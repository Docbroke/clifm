/* suggestions.c -- functions to manage the suggestions system */

/*
 * This file is part of CliFM
 * 
 * Copyright (C) 2016-2021, L. Abramovich <johndoe.arch@outlook.com>
 * All rights reserved.

 * CliFM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * CliFM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
*/
#ifndef _NO_SUGGESTIONS

#include "helpers.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#ifdef __OpenBSD__
#include <strings.h>
#endif
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <termios.h>

#ifdef __linux__
#include <sys/capability.h>
#endif

#ifdef __OpenBSD__
typedef char *rl_cpvfunc_t;
#include <ereadline/readline/readline.h>
#else
#include <readline/readline.h>
#endif

#include "suggestions.h"
#include "aux.h"
#include "checks.h"
#include "colors.h"
#include "jump.h"
#include "readline.h"

static int free_color = 0;

/* The following three functions were taken from
 * https://github.com/antirez/linenoise/blob/master/linenoise.c
 * and modified to fir our needs: they are used to get current cursor
 * position (both vertical and horizontal) by the suggestions system */

/* Set the terminal into raw mode. Return 0 on success and -1 on error */
static int
enable_raw_mode(const int fd)
{
	struct termios raw;

	if (!isatty(STDIN_FILENO))
		goto FAIL;

	if (tcgetattr(fd, &orig_termios) == -1)
		goto FAIL;

	raw = orig_termios;  /* modify the original mode */
	/* input modes: no break, no CR to NL, no parity check, no strip char,
	 * * no start/stop output control. */
	raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	/* output modes - disable post processing */
	raw.c_oflag &= (tcflag_t)~(OPOST);
	/* control modes - set 8 bit chars */
	raw.c_cflag |= (CS8);
	/* local modes - choing off, canonical off, no extended functions,
	 * no signal chars (^Z,^C) */
	raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
	raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

	/* put terminal in raw mode after flushing */
	if (tcsetattr(fd, TCSAFLUSH, &raw) < 0)
		goto FAIL;

	return 0;

FAIL:
	errno = ENOTTY;
	return -1;
}

static int
disable_raw_mode(const int fd)
{
	if (tcsetattr(fd, TCSAFLUSH, &orig_termios) != -1)
		return EXIT_SUCCESS;
	return EXIT_FAILURE;
}

/* Use the "ESC [6n" escape sequence to query the cursor position (both
 * vertical and horizontal) and store both values into global variables.
 * Return 0 on success and 1 on error */
static int
get_cursor_position(const int ifd, const int ofd)
{
	char buf[32];
	int cols, rows;
	unsigned int i = 0;

	if (enable_raw_mode(ifd) == -1)
		return EXIT_FAILURE;

	/* Report cursor location */
	if (write(ofd, "\x1b[6n", 4) != 4)
		goto FAIL;

	/* Read the response: "ESC [ rows ; cols R" */
	while (i < sizeof(buf) - 1) {
		if (read(ifd, buf + i, 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';

	/* Parse it */
	if (buf[0] != _ESC || buf[1] != '[')
		goto FAIL;
	if (sscanf(buf + 2, "%d;%d", &rows, &cols) != 2)
		goto FAIL;

	currow = rows;
	curcol = cols;

	disable_raw_mode(ifd);
	return EXIT_SUCCESS;

FAIL:
	disable_raw_mode(ifd);
	return EXIT_FAILURE;
}

/* This function is only used before running a keybind command. We don't
 * want the suggestion buffer after running a keybind */
void
free_suggestion(void)
{
	free(suggestion_buf);
	suggestion_buf = (char *)NULL;
	suggestion.printed = 0;
	suggestion.nlines = 0;
}

void
clear_suggestion(void)
{
	/* Delete everything in the current line starting from the current
	 * cursor position */
	if (write(STDOUT_FILENO, DLFC, DLFC_LEN) <= 0) {}

	if (suggestion.nlines > 1) {
		/* Save cursor position */
		get_cursor_position(STDIN_FILENO, STDOUT_FILENO);

		int i = (int)suggestion.nlines;
		while (--i > 0) {
			/* Move the cursor to the beginning of the next line */
			if (write(STDOUT_FILENO, "\x1b[1E", 4) <= 0) {}
			/* Delete the line */
			if (write(STDOUT_FILENO, "\x1b[0K", 4) <= 0) {}
		}
		/* Restore cursor position */
		printf("\x1b[%d;%dH", currow, curcol);
		fflush(stdout);
		suggestion.nlines = 0;
	}

	suggestion.printed = 0;
}

static void
remove_suggestion_not_end(void)
{
	printf("\x1b[%dC", rl_end - rl_point);
	fflush(stdout);
	clear_suggestion();
	printf("\x1b[%dD", rl_end - rl_point);
	fflush(stdout);
}

/* Clear the line, print the suggestion (STR) at OFFSET in COLOR, and
 * move the cursor back to the original position.
 * OFFSET marks the point in STR that is already typed: the suggestion
 * will be printed starting from this point */
static void
print_suggestion(const char *str, size_t offset, const char *color)
{
	if (suggestion.printed)
		clear_suggestion();

	free(suggestion_buf);
	suggestion_buf = (char *)NULL;

	/* Store cursor position in two global variables: currow and curcol */
	get_cursor_position(STDIN_FILENO, STDOUT_FILENO);

	/* Do not print suggestions bigger than what the current terminal
	 * window size can hold */
	size_t suggestion_len = wc_xstrlen(str + offset);
	if ((int)suggestion_len > (term_cols * term_rows) - curcol)
		return;

	size_t cuc = (size_t)curcol; /* Current cursor column position*/
	int baej = 0; /* Bookmark, alias, ELN, or jump */

	if (suggestion.type == BOOKMARK_SUG || suggestion.type == ALIAS_SUG
	|| suggestion.type == ELN_SUG || suggestion.type == JCMD_SUG
	|| suggestion.type == JCMD_SUG_NOACD) {
		/* 4 = 2 (two chars forward) + 2 (" >") */
		cuc += 4;
		baej = 1;
	}

	size_t cucs = cuc + suggestion_len;
	/* slines: amount of lines we need to print the suggestion, including
	 * the current line */
	size_t slines = 1;

	if (cucs > term_cols) {
		slines = cucs / (size_t)term_cols;
		int cucs_rem = (int)cucs % term_cols;
		if (cucs_rem > 0)
			slines++;
	}

	if (slines > (size_t)term_rows)
		return;

	/* Store the suggestion in a buffer to be used later by the
	 * rl_accept_suggestion function (keybinds.c) */
	suggestion_buf = xnmalloc(strlen(str) + 1, sizeof(char));
	strcpy(suggestion_buf, str);

	/* Erase everything after the current cursor position */
	if (write(STDOUT_FILENO, DLFC, DLFC_LEN) <= 0) {}

	/* If not at the end of the line, move the cursor there */
	if (rl_end > rl_point)
		printf("\x1b[%dC", rl_end - rl_point);
	/* rl_end and rl_point are not updated: they do not include
	 * the last typed char. However, since we only care here about
	 * the difference between them, it doesn't matter: the result
	 * is the same (7 - 4 == 6 - 3 == 1) */

	if (baej) {
		/* Move the cursor two columns to the right and print "> " */
		fputs("\x1b[2C", stdout);
		printf("%s> \x1b[0m", mi_c);
	}

	/* Print the suggestion */
//	printf("%s%s%s", color, str + offset - (offset ? 1 : 0), df_c);
	printf("%s%s", color, str + offset - (offset ? 1 : 0));
	fflush(stdout);

	/* Update the row number, if needed */
	/* If the cursor is in the last row, printing a multi-line suggestion
	 * will move the beginning of the current line up the number of
	 * lines taken by the suggestion, so that we need to update the
	 * value to move the cursor back to the correct row (the beginning
	 * of the line) */
	int old_currow = currow;
	/* extra_rows: amount of extra rows we need to print the suggestion
	 * (excluding the current row) */
	int extra_rows = (int)slines - 1;
	if (extra_rows && old_currow + extra_rows >= term_rows)
		currow -= extra_rows - (term_rows - old_currow);

	/* Restore cursor position */
	printf("\x1b[%d;%dH", currow, curcol);

	/* Store the amount of lines taken by the current command line
	 * (plus the suggestion's length) to be able to correctly
	 * remove it later (via the clear_suggestion function) */
	suggestion.nlines = slines;
	return;
}

/* Used by the check_completions function to get file names color
 * according to file type */
static char *
get_comp_color(const char *filename, const struct stat attr)
{
	char *color = no_c; 

	switch(attr.st_mode & S_IFMT) {
	case S_IFDIR:
		if (light_mode)
			return di_c;
		if (access(filename, R_OK | X_OK) != 0) {
			color = nd_c;
		} else {
			int sticky = 0;
			int is_oth_w = 0;
			if (attr.st_mode & S_ISVTX)
				sticky = 1;

			if (attr.st_mode & S_IWOTH)
				is_oth_w = 1;

			int files_dir = count_dir(filename, CPOP);

			color = sticky ? (is_oth_w ? tw_c : st_c) : is_oth_w ? ow_c
				   : ((files_dir == 2 || files_dir == 0) ? ed_c : di_c);
		}
		break;

	case S_IFREG:
		if (light_mode)
			return fi_c;
		if (access(filename, R_OK) == -1)
			color = nf_c;
		else if (attr.st_mode & S_ISUID)
			color = su_c;
		else if (attr.st_mode & S_ISGID)
			color = sg_c;
		else {
#ifdef _LINUX_CAP
			cap_t cap = cap_get_file(filename);
			if (cap) {
				color = ca_c;
				cap_free(cap);
			} else if (attr.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
#else
			if (attr.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
#endif
				if (attr.st_size == 0)
					color = ee_c;
				else
					color = ex_c;
			} else if (attr.st_size == 0)
				color = ef_c;
			else if (attr.st_nlink > 1)
				color = mh_c;
			else {
				char *ext = strrchr(filename, '.');
				if (ext && ext != filename) {
					char *extcolor = get_ext_color(ext);
					if (extcolor) {
						char *ext_color = (char *)xnmalloc(strlen(extcolor)
											+ 4, sizeof(char));
						sprintf(ext_color, "\x1b[%sm", extcolor);
						color = ext_color;
						free_color = 1;
						extcolor = (char *)NULL;
					} else  {
						color = fi_c;
					}
				} else {
					color = fi_c;
				}
			}
		}
		break;

	case S_IFLNK: {
		if (light_mode)
			return ln_c;
		char *linkname = realpath(filename, (char *)NULL);
		if (linkname)
			color = ln_c;
		else
			color = or_c;
		}
		break;

	case S_IFSOCK: color = so_c; break;
	case S_IFBLK: color = bd_c; break;
	case S_IFCHR: color = cd_c; break;
	case S_IFIFO: color = pi_c; break;
	default: color = no_c; break;
	}

	return color;
}

static int
check_completions(const char *str, size_t len, const unsigned char c)
{
	int printed = 0;
	size_t i;
	struct stat attr;
	char **_matches = rl_completion_matches(str, rl_completion_entry_function);

	suggestion.filetype = DT_REG;
	free_color = 0;

	char *color = (char *)NULL, *_color = (char *)NULL;
	if (suggest_filetype_color)
		color = no_c;
	else
		color = sf_c;

	if (!_matches)
		return printed;

	if (!len)
		goto FREE;

	/* If only one match */
	if (_matches[0] && *_matches[0]	&& strlen(_matches[0]) > len) {
		int append_slash = 0;

		char *p = (char *)NULL;
		if (*_matches[0] == '~')
			p = tilde_expand(_matches[0]);

		if (lstat(p ? p : _matches[0], &attr) != -1) {
			if ((attr.st_mode & S_IFMT) == S_IFDIR) {
				append_slash = 1;
				suggestion.filetype = DT_DIR;
			}
			if (suggest_filetype_color)
				color = get_comp_color(p ? p : _matches[0], attr);
		} else {
			/* We have a partial completion. Set filetype to DT_DIR
			 * so that the rl_accept_suggestion function won't append
			 * a space after the file name */
			suggestion.filetype = DT_DIR;
		}

		free(p);

		char t[NAME_MAX + 2];
		*t = '\0';
		if (append_slash)
			snprintf(t, NAME_MAX + 2, "%s/", _matches[0]);
		char *tmp = escape_str(*t ? t : _matches[0]);

		if (c != BS)
			suggestion.type = COMP_SUG;

		if (tmp) {
			print_suggestion(tmp, len, color);
			free(tmp);
		} else {
			print_suggestion(_matches[0], len, color);
		}

		printed = 1;
	} else {
		/* If multiple matches, suggest the first one */
		if (_matches[1] && *_matches[1]
		&& strlen(_matches[1]) > len) {
			int append_slash = 0;

			char *p = (char *)NULL;
			if (*_matches[1] == '~')
				p = tilde_expand(_matches[1]);

			if (lstat(p ? p : _matches[1], &attr) != -1) {
				if ((attr.st_mode & S_IFMT) == S_IFDIR) {
					append_slash = 1;
					suggestion.filetype = DT_DIR;
				}

				if (suggest_filetype_color) {
					_color = get_comp_color(p ? p : _matches[1], attr);
					if (_color)
						color = _color;
				}
			} else {
				suggestion.filetype = DT_DIR;
			}

			free(p);

			char _tmp[NAME_MAX + 2];
			*_tmp = '\0';
			if (append_slash)
				snprintf(_tmp, NAME_MAX + 2, "%s/", _matches[1]);
			char *tmp = escape_str(*_tmp ? _tmp : _matches[1]);

			if (c != BS)
				suggestion.type = COMP_SUG;

			if (tmp) {
				print_suggestion(tmp, len, color);
				free(tmp);
			} else {
				print_suggestion(_matches[1], len, color);
			}

			printed = 1;
		}
	}

FREE:
	for (i = 0; _matches[i]; i++)
		free(_matches[i]);
	free(_matches);

	if (free_color) {
		free(_color);
		free_color = 0;
	}

	return printed;
}

static int
check_filenames(const char *str, const size_t len, const unsigned char c, const int first_word)
{
	int i = (int)files;
	char *color = (char *)NULL;

	if (suggest_filetype_color)
		color = no_c;
	else
		color = sf_c;

	while (--i >= 0) {
		if (!file_info[i].name || TOUPPER(*str) != TOUPPER(*file_info[i].name))
			continue;
		if (len && (case_sens_path_comp	? strncmp(str, file_info[i].name, len)
		: strncasecmp(str, file_info[i].name, len)) == 0
		&& file_info[i].len > len) {
			if (suggest_filetype_color)
				color = file_info[i].color;

			if (file_info[i].dir) {
				if (first_word && !autocd)
					continue;

				suggestion.filetype = DT_DIR;

				char tmp[NAME_MAX + 2];
				snprintf(tmp, NAME_MAX + 2, "%s/", file_info[i].name);

				if (c != BS)
					suggestion.type = FILE_SUG;

				char *_tmp = escape_str(tmp);
				if (_tmp) {
					print_suggestion(_tmp, len, color);
					free(_tmp);
				} else {
					print_suggestion(tmp, len, color);
				}
			} else {
				if (first_word && !auto_open)
					continue;

				if (c != BS)
					suggestion.type = FILE_SUG;
				suggestion.filetype = DT_REG;

				char *tmp = escape_str(file_info[i].name);
				if (tmp) {
					print_suggestion(tmp, len, color);
					free(tmp);
				} else {
					print_suggestion(file_info[i].name, len, color);
				}
			}
			return 1;
		}
	}
	return 0;
}

static int
check_history(const char *str, const size_t len)
{
	if (!str || !*str || !len)
		return 0;

	int i = (int)current_hist_n;
	while (--i >= 0) {
		if (!history[i] || TOUPPER(*str) != TOUPPER(*history[i]))
			continue;

		if ((case_sens_path_comp ? strncmp(str, history[i], len)
		: strncasecmp(str, history[i], len)) == 0
		&& strlen(history[i]) > len) {
			suggestion.type = HIST_SUG;
			print_suggestion(history[i], len, sh_c);
			return 1;
		}
	}

	return 0;
}

static int
check_cmds(const char *str, const size_t len)
{
	int i = (int)path_progsn;
	while (--i >= 0) {
		if (!bin_commands[i] || *str != *bin_commands[i])
			continue;

		if (len && strncmp(str, bin_commands[i], len) == 0
		&& strlen(bin_commands[i]) > len) {
			if (is_internal_c(bin_commands[i])) {
				suggestion.type = CMD_SUG;
				print_suggestion(bin_commands[i], len, sx_c);
			} else if (ext_cmd_ok) {
				suggestion.type = CMD_SUG;
				print_suggestion(bin_commands[i], len, sc_c);
			} else {
				continue;
			}
			return 1;
		}
	}

	return 0;
}

static int
check_jumpdb(const char *str, const size_t len)
{
	char *color = (char *)NULL;

	if (suggest_filetype_color)
		color = di_c;
	else
		color = sf_c;

	int i = (int)jump_n;
	while (--i >= 0) {
		if (!jump_db[i].path || TOUPPER(*str) != TOUPPER(*jump_db[i].path))
			continue;

		size_t db_len = strlen(jump_db[i].path);
		if (len && (case_sens_path_comp ? strncmp(str, jump_db[i].path, len)
		: strncasecmp(str, jump_db[i].path, len)) == 0
		&& db_len > len) {
			suggestion.type = FILE_SUG;
			suggestion.filetype = DT_DIR;
			char tmp[NAME_MAX + 2];
			*tmp = '\0';
			if (jump_db[i].path[db_len - 1] != '/')
				snprintf(tmp, NAME_MAX + 2, "%s/", jump_db[i].path);
			print_suggestion(*tmp ? tmp : jump_db[i].path, len, color);
			return 1;
		}
	}

	return 0;
}

static int
check_bookmarks(const char *str, const size_t len)
{
	if (!bm_n)
		return 0;

	char *color = (char *)NULL;
	struct stat attr;
	if (!suggest_filetype_color)
		color = sf_c;

	int i = (int)bm_n;
	while (--i >= 0) {
		if (!bookmarks[i].name || TOUPPER(*str) != TOUPPER(*bookmarks[i].name))
			continue;

		if (len && (case_sens_path_comp ? strncmp(str, bookmarks[i].name, len)
		: strncasecmp(str, bookmarks[i].name, len)) == 0) {
			if (lstat(bookmarks[i].path, &attr) == -1)
				continue;

			else if ((attr.st_mode & S_IFMT) == S_IFDIR) {
				suggestion.type = BOOKMARK_SUG;
				suggestion.filetype = DT_DIR;

				char tmp[PATH_MAX + 2];
				size_t path_len = strlen(bookmarks[i].path);
				if (bookmarks[i].path[path_len - 1] != '/')
					snprintf(tmp, PATH_MAX + 2, "%s/", bookmarks[i].path);
				else
					xstrsncpy(tmp, bookmarks[i].path, PATH_MAX + 2);

				if (suggest_filetype_color)
					color = di_c;

				char *_tmp = escape_str(tmp);
				print_suggestion(_tmp ? _tmp : tmp, 1, color);
				if (_tmp)
					free(_tmp);
			} else {
				suggestion.type = BOOKMARK_SUG;
				suggestion.filetype = DT_REG;

				if (suggest_filetype_color)
					color = get_comp_color(bookmarks[i].path, attr);

				char *_tmp = escape_str(bookmarks[i].path);
				print_suggestion(_tmp ? _tmp : bookmarks[i].path, 1, color);
				if (_tmp)
					free(_tmp);
			}
			return 1;
		}
	}

	return 0;
}

/*
static int
check_bookmarks(const char *str, const size_t len)
{
	int i = bm_n;
	char *color = (char *)NULL;
	struct stat attr;
	if (!suggest_filetype_color)
		color = sf_c;

	while (--i >= 0) {
		if (!bookmarks[i].path || *str != *bookmarks[i].path)
			continue;
		if (len && strncmp(str, bookmarks[i].path, len) == 0
		&& strlen(bookmarks[i].path) > len) {
			if (lstat(bookmarks[i].path, &attr) == -1)
				continue;
			else if ((attr.st_mode & S_IFMT) == S_IFDIR) {
				char tmp[NAME_MAX + 2];
				snprintf(tmp, NAME_MAX + 2, "%s/", bookmarks[i].path);
				if (suggest_filetype_color)
					color = di_c;
				char *_tmp = escape_str(tmp);
				print_suggestion(_tmp ? _tmp : tmp, len, color);
				if (_tmp)
					free(_tmp);
				suggestion.type = FILE_SUG;
				suggestion.filetype = DT_DIR;
			} else {
				if (suggest_filetype_color)
					color = get_comp_color(bookmarks[i].path, attr);
				char *_tmp = escape_str(bookmarks[i].path);
				print_suggestion(_tmp ? _tmp : bookmarks[i].path, len,
								color);
				if (_tmp)
					free(_tmp);
				print_suggestion(bookmarks[i].path, len, color);
				suggestion.type = FILE_SUG;
				suggestion.filetype = DT_REG;
			}
			return 1;
		}
	}

	return 0;
} */

static int
check_int_params(const char *str, const size_t len)
{
	size_t i;
	for (i = 0; param_str[i]; i++) {
		if (*str != *param_str[i])
			continue;
		if (len && strncmp(str, param_str[i], len) == 0
		&& strlen(param_str[i]) > len) {
			suggestion.type = INT_CMD;
			print_suggestion(param_str[i], len, sx_c);
			return 1;
		}
	}

	return 0;
}

static int
check_eln(const char *str)
{
	if (!str || !*str)
		return 0;

	int n = atoi(str);
	if (n < 1 || n > (int)files || !file_info[n - 1].name)
		return 0;

	n--;
	char *color = sf_c;
	suggestion.type = ELN_SUG;
	if (suggest_filetype_color)
		color = file_info[n].color;

	char tmp[NAME_MAX + 1];
	*tmp = '\0';
	if (file_info[n].dir) {
		snprintf(tmp, NAME_MAX + 1, "%s/", file_info[n].name);
		suggestion.filetype = DT_DIR;
	} else {
		suggestion.filetype = DT_REG;
	}

	print_suggestion(!*tmp ? file_info[n].name : tmp, 1, color);
	return 1;
}

static int
check_aliases(const char *str, const size_t len)
{
	if (!aliases_n)
		return 0;

	char *color = sc_c;

	int i = (int)aliases_n;
	while (--i >= 0) {
		if (!aliases[i].name)
			continue;
		char *p = aliases[i].name;
		if (TOUPPER(*p) != TOUPPER(*str))
			continue;
		if ((case_sens_path_comp ? strncmp(p, str, len)
		: strncasecmp(p, str, len)) != 0)
			continue;
		if (!aliases[i].cmd || !*aliases[i].cmd)
			continue;

		suggestion.type = ALIAS_SUG;
		print_suggestion(aliases[i].cmd, 1, color);
		return 1;
	}

	return 0;
}

/* Get a match from the jump database and print the suggestion */
static int
check_jcmd(char *line)
{
	if (suggestion_buf)
		clear_suggestion();

	/* Split line into an array of substrings */
	char **substr = get_substr(line, ' ');
	if (!substr)
		return 0;

	/* Check the jump database for a match. If a match is found, it will
	 * be stored in jump_suggestion (global) */
	dirjump(substr, SUG_JUMP);

	size_t i;
	for (i = 0; substr[i]; i++)
		free(substr[i]);
	free(substr);

	if (!jump_suggestion)
		return 0;

	suggestion.type = JCMD_SUG;
	suggestion.filetype = DT_DIR;
	if (!autocd) {
		char *tmp = xnmalloc(strlen(jump_suggestion) + 4, sizeof(char));
		sprintf(tmp, "cd %s", jump_suggestion);
		print_suggestion(tmp, 1, suggest_filetype_color ? di_c : sf_c);
		suggestion.type = JCMD_SUG_NOACD;
		free(tmp);
	} else {
		print_suggestion(jump_suggestion, 1, suggest_filetype_color ? di_c
					: sf_c);
	}
	suggestion.offset = 0;
	free(jump_suggestion);
	jump_suggestion = (char *)NULL;
	return 1;
}

/* Check if we must suggest --help for internal commands */
static int
check_help(char *full_line, const char *last_word)
{
	size_t len = strlen(last_word);
	if (strncmp(last_word, "--help", len) != 0)
		return 0;

	char *ret = strchr(full_line, ' ');
	if (!ret)
		return 0;

	*ret = '\0';
	if (!is_internal_c(full_line))
		return 0;

	suggestion.type = CMD_SUG;
	print_suggestion("--help", len, sx_c);
	return 1;
}

static int
check_variables(const char *str, const size_t len)
{
	int printed = 0;
	size_t i;
	for (i = 0; environ[i]; i++) {
		if (TOUPPER(*environ[i]) == TOUPPER(*str)
		&& strncasecmp(str, environ[i], len) == 0) {
			char *ret = strchr(environ[i], '=');
			*ret = '\0';
			suggestion.type = VAR_SUG;
			char t[NAME_MAX + 1];
			snprintf(t, NAME_MAX + 1, "$%s", environ[i]);
			print_suggestion(t, len + 1, sh_c);
			printed = 1;
			*ret = '=';
			break;
		}
	}

	if (printed)
		return 1;

	if (!usrvar_n)
		return 0;

	for (i = 0; usr_var[i].name; i++) {
		if (TOUPPER(*str) == TOUPPER(*usr_var[i].name)
		&& strncasecmp(str, usr_var[i].name, len) == 0) {
			suggestion.type = CMD_SUG;
			char t[NAME_MAX + 1];
			snprintf(t, NAME_MAX + 1, "$%s", usr_var[i].name);
			print_suggestion(t, len + 1, sh_c);
			printed = 1;
			break;
		}
	}

	if (printed)
		return 1;

	return 0;
}

/* Check for available suggestions. Returns zero if true, one if not,
 * and -1 if C was inserted before the end of the current line.
 * If a suggestion is found, it will be printed by print_suggestion() */
int
rl_suggestions(const unsigned char c)
{
	char *last_word = (char *)NULL;
	char *full_line = (char *)NULL;
	int printed = 0;
	int inserted_c = 0;
/*	static int msg_area = 0; */

		/* ######################################
		 * # 		  1) Filter input			#
		 * ######################################*/

	/* Disable suggestions while in vi command mode and reenable them
	 * when changing back to insert mode */
	if (rl_editing_mode == 0) {
		if (rl_readline_state & RL_STATE_VICMDONCE) {
			if (c == 'i') {
				rl_readline_state &= (unsigned long)~RL_STATE_VICMDONCE;
			} else if (suggestion.printed) {
				clear_suggestion();
				free(suggestion_buf);
				suggestion_buf = (char *)NULL;
				goto FAIL;
			} else {
				goto FAIL;
			}
		}
	}

	/* Skip escape sequences, mostly arrow keys */
	if (rl_readline_state & RL_STATE_MOREINPUT) {
		if (c == '~') {
			if (rl_point != rl_end && suggestion.printed) {
				/* This should be the delete key */
				remove_suggestion_not_end();
				goto FAIL;
			} else if (suggestion.printed) {
				clear_suggestion();
				free(suggestion_buf);
				suggestion_buf = (char *)NULL;
			}
		}
		/* Handle history events. If a suggestion has been printed and
		 * a history event is triggered (usually via the Up and Down arrow
		 * keys), the suggestion buffer won't be freed. Let's do it
		 * here */
		else if ((c == 'A' || c == 'B') && suggestion_buf) {
			clear_suggestion();
			goto FAIL;
		}
		if (suggestion_buf) {
			printed = 1;
			goto SUCCESS;
		}
		goto FAIL;
	}

	/* Skip control characters (0 - 31) except backspace (8), tab(9),
	 * enter (13), and escape (27) */
	if (c < 32 && c != BS && c != _TAB && c != ENTER && c != _ESC) {
		if (suggestion_buf) {
			printed = 1;
			goto SUCCESS;
		} else {
			goto FAIL;
		}
	}

	/* Skip backspace, Enter, and TAB keys */
	switch(c) {
		case DELETE: /* fallthrough */
/*			if (rl_point != rl_end && suggestion.printed)
				clear_suggestion();
			goto FAIL; */

		case BS:
			if (suggestion.printed && suggestion_buf) {
				if (rl_point != rl_end) {
					remove_suggestion_not_end();
				} else {
					clear_suggestion();
				}
			}
			goto FAIL;

		case ENTER:
			if (suggestion.printed && suggestion_buf)
				clear_suggestion();
			goto FAIL;
/*		case SPACE:
			if (msg_area) {
				rl_restore_prompt();
				rl_clear_message();
				msg_area = 0;
				if (c == ENTER)
					goto FAIL;
				break;
			} else {
				if (c == ENTER)
					goto FAIL;
				break;
			} */

		case _ESC:
			if (suggestion.printed)
				printed = 1;
			goto SUCCESS;

		case _TAB:
			if (suggestion.printed) {
				if (suggestion.nlines < 2 && suggestion.type != ELN_SUG
				&& suggestion.type != BOOKMARK_SUG
				&& suggestion.type != ALIAS_SUG
				&& suggestion.type != JCMD_SUG) {
					printed = 1;
				} else {
					clear_suggestion();
					goto FAIL;
				}
			}
			goto SUCCESS;

		default: break;
	}

		/* ######################################
		 * #	2) Handle last entered char		#
		 * ######################################*/

	/* If not at the end of line, insert C in the current cursor
	 * position. Else, append it to current readline buffer to
	 * correctly find matches: at this point (rl_getc), readline has
	 * not appended this char to rl_line_buffer yet, so that we must
	 * do it manually.
	 * Line editing is only allowed for the last word */
	int s = strcntchrlst(rl_line_buffer, ' ');
	/* Do not take into account final spaces */
	if (s >= 0 && !rl_line_buffer[s + 1])
		s = -1;
	if (rl_point != rl_end && c != _ESC) {
		if (rl_point < s) {
			if (suggestion.printed)
				remove_suggestion_not_end();
			goto FAIL;
		}
		char text[2];
		text[0] = (char)c;
		text[1] = '\0';
		rl_insert_text(text);
		/* This flag is used to tell my_rl_getc not to append C to the
		 * line buffer (rl_line_buffer), since it was already inserted
		 * here */
		inserted_c = 1;
	}

	size_t buflen = (size_t)rl_end;
/*	size_t buflen = strlen(rl_line_buffer); */
	suggestion.full_line_len = buflen + 1;
	char *last_space = strrchr(rl_line_buffer, ' ');
	if (last_space && last_space != rl_line_buffer
	&& *(last_space - 1) == '\\')
		last_space = (char *)NULL;

	/* We need a copy of the complete line */
	full_line = (char *)xnmalloc(buflen + 2, sizeof(char));
	if (inserted_c)
		strcpy(full_line, rl_line_buffer);
	else
		sprintf(full_line, "%s%c", rl_line_buffer, c);

	/* And a copy of the last entered word as well */
	int last_word_offset = 0;

	if (last_space) {
		int j = (int)buflen;
		while (--j >= 0) {
			if (rl_line_buffer[j] == ' ')
				break;
		}
		last_word_offset = j + 1;
		buflen = strlen(last_space);

		if (*(++last_space)) {
			buflen--;
			last_word = (char *)xnmalloc(buflen + 2, sizeof(char));
			if (inserted_c)
				strcpy(last_word, last_space);
			else
				sprintf(last_word, "%s%c", last_space, c);
		} else {
			last_word = (char *)xnmalloc(2, sizeof(char));
			if (inserted_c) {
				*last_word = '\0';
			} else {
				*last_word = (char)c;
				last_word[1] = '\0';
			}
		}
	} else {
		last_word = (char *)xnmalloc(buflen + 2, sizeof(char));
		if (inserted_c)
			strcpy(last_word, rl_line_buffer);
		else
			sprintf(last_word, "%s%c", rl_line_buffer, c);
	}

		/* ######################################
		 * #	  3) Search for suggestions		#
		 * ######################################*/

	char *lb = rl_line_buffer;
	/* 3.a) Let's suggest non-fixed parameters for internal commands */

	switch(*lb) {
	case 'b': /* Bookmarks names */
		if (lb[1] == 'm' && lb[2] == ' ' && strncmp(lb + 3, "add", 3) != 0) {
			size_t i = 0, len = strlen(last_word);
			for (; bookmark_names[i]; i++) {
				if (*last_word == *bookmark_names[i]
				&& strncmp(bookmark_names[i], last_word, len) == 0) {
					suggestion.type = CMD_SUG;
					suggestion.offset = last_word_offset;
					print_suggestion(bookmark_names[i], len, sx_c);
					printed = 1;
					break;
				}
			}
			if (printed) {
				goto SUCCESS;
			}
		}
		break;

	case 'c': /* Color schemes */
		if (lb[1] == 's' && lb[2] == ' ') {
			size_t i = 0, len = strlen(last_word);
			for (; color_schemes[i]; i++) {
				if (*last_word == *color_schemes[i]
				&& strncmp(color_schemes[i], last_word, len) == 0) {
					suggestion.type = CMD_SUG;
					suggestion.offset = last_word_offset;
					print_suggestion(color_schemes[i], len, sx_c);
					printed = 1;
					break;
				}
			}
			if (printed) {
				goto SUCCESS;
			}
		}
		break;

	case 'j': /* j command */
		if (lb[1] == ' '  || ((lb[1] == 'c'	|| lb[1] == 'o'
		|| lb[1] == 'p') && lb[2] == ' ')) {
			printed = check_jcmd(full_line);
			if (printed) {
				goto SUCCESS;
			} else {
				free(full_line);
				full_line = (char *)NULL;
				goto FAIL;
			}
		}
		break;

	case 'n': /* Remotes */
		if (lb[1] == 'e' && lb[2] == 't' && lb[3] == ' ') {
			size_t i = 0, len = strlen(last_word);
			for (; remotes[i].name; i++) {
				if (*last_word == *remotes[i].name
				&& strncmp(remotes[i].name, last_word, len) == 0) {
					suggestion.type = CMD_SUG;
					suggestion.offset = last_word_offset;
					print_suggestion(remotes[i].name, len, sx_c);
					printed = 1;
					break;
				}
			}
			if (printed)
				goto SUCCESS;
		}
		break;

	case 'p': /* Profiles */
		if (lb[1] == 'f' && lb[2] == ' ' && (strncmp(lb + 3, "set", 3) == 0
		|| strncmp(lb + 3, "del", 3) == 0)) {
			size_t i = 0, len = strlen(last_word);
			for (; profile_names[i]; i++) {
				if (*last_word == *profile_names[i]
				&& strncmp(profile_names[i], last_word, len) == 0) {
					suggestion.type = CMD_SUG;
					suggestion.offset = last_word_offset;
					print_suggestion(profile_names[i], len, sx_c);
					printed = 1;
					break;
				}
			}
			if (printed) {
				goto SUCCESS;
			} else {
				free(full_line);
				full_line = (char *)NULL;
				goto FAIL;
			}
		}
		break;

	default: break;
	}

	/* 3.b) Check already suggested string */
	if (suggestion_buf && suggestion.printed && !_ISDIGIT(c)
	&& strncmp(full_line, suggestion_buf, strlen(full_line)) == 0) {
		printed = 1;
		suggestion.offset = 0;
		goto SUCCESS;
	}

	/* 3.c) Check CliFM internal parameters */
	char *ret = strchr(full_line, ' ');
	if (ret) {
		size_t len = strlen(last_word);
		/* 3.c.1) Suggest the sel keyword only if not first word */
		if (*last_word == 's' && strncmp(last_word, "sel", len) == 0) {
			suggestion.type = CMD_SUG;
			suggestion.offset = last_word_offset;
			printed = 1;
			print_suggestion("sel", len, sx_c);
			goto SUCCESS;
		}

		/* 3.c.2) Check commands fixed parameters */
		printed = check_int_params(full_line, strlen(full_line));
		if (printed) {
			suggestion.offset = 0;
			goto SUCCESS;
		}
	}

	/* 3.c.3) Let's suggest --help for internal commands */
	if (*last_word == '-') {
		printed = check_help(full_line, last_word);
		if (printed) {
			suggestion.offset = last_word_offset;
			goto SUCCESS;
		}
	}

	/* 3.d) Execute the following check in the order specified by
	 * suggestion_strategy (the value is taken form the configuration
	 * file) */
	size_t st = 0;
	for (; st < SUG_STRATS; st++) {
		switch(suggestion_strategy[st]) {

		case 'a': /* 3.d.1) Aliases */
			printed = check_aliases(last_word, strlen(last_word));
			if (printed) {
				suggestion.offset = last_word_offset;
				goto SUCCESS;
			}
			break;

		case 'b': /* 3.d.2) Bookmarks */
			if (last_space || autocd || auto_open) {
				printed = check_bookmarks(last_word, strlen(last_word));
				if (printed) {
					suggestion.offset = last_word_offset;
					goto SUCCESS;
				}
			}
			break;

		case 'c': /* 3.d.3) Possible completions */
			if (last_space || autocd || auto_open) {
				printed = check_completions(last_word, strlen(last_word), c);
				if (printed) {
					suggestion.offset = last_word_offset;
					goto SUCCESS;
				}
			}
			break;

		case 'e': /* 3.d.4) ELN's */
			if (*last_word >= '1' && *last_word <= '9' && is_number(last_word)) {
				printed = check_eln(last_word);
				if (printed) {
					suggestion.offset = last_word_offset;
					goto SUCCESS;
				}
			}
			break;

		case 'f': /* 3.d.5) File names in CWD */
			/* Do not check dirs and filenames if first word and
			 * neither autocd nor auto-open are enabled */
			if (last_space || autocd || auto_open) {
				printed = check_filenames(last_word, strlen(last_word),
							c, last_space ? 0 : 1);
				if (printed) {
					suggestion.offset = last_word_offset;
					goto SUCCESS;
				}
			}
			break;

		case 'h': /* 3.d.6) Commands history */
			printed = check_history(full_line, strlen(full_line));
			if (printed) {
				suggestion.offset = 0;
				goto SUCCESS;
			}
			break;

		case 'j': /* 3.d.7) Jump database */
			/* We don't care about auto-open here: the jump function
			 * deals with directories only */
			if (last_space || autocd) {
				printed = check_jumpdb(last_word, strlen(last_word));
				if (printed) {
					suggestion.offset = last_word_offset;
					goto SUCCESS;
				}
			}
			break;

		case '-': break; /* Ignore check */

		default: break;
		}
	}

	/* 3.e) Variable names, both environment and internal */
	if (*last_word == '$') {
		printed = check_variables(last_word + 1, strlen(last_word + 1));
		if (printed) {
			suggestion.offset = last_word_offset;
			goto SUCCESS;
		}
	}

	/* 3.f) Check commands in PATH and CliFM internals commands, but
	 * only for the first word */
	if (!last_space) {
		size_t w_len = strlen(last_word);
		printed = check_cmds(last_word, w_len);
		if (printed) {
			suggestion.offset = 0;
			goto SUCCESS;
		} /*else {
			static int wrong = 0;
			if (wrong)
				goto FAIL;
			wrong = 1;
//			int bk = rl_point;
			fputs("\x1b[0;31m", stdout);
			rl_point = 0;
			rl_delete_text(0, rl_end);
//			rl_point = rl_end = 0;
//			printf("\x1b[%zuD", w_len);
//			fputs("\x1b[0;31m", stdout);
//			printf("\x1b[%zuC", w_len);
			rl_insert_text(last_word);
//			rl_point = bk;
			inserted_c = 1;
//			rl_point = bk;
//			rl_redisplay();
//			rl_point = bk;
//			rl_end = rl_point = 0; 
//			cur_color = nf_c;
//			fputs("\x1b[0;31m", stdout);
//			fflush(stdout);
//			last_word[w_len - 1] = '\0';
//			rl_insert_text(last_word);
//			printed = 1;
			goto FAIL;
		} */
	}

	/* No suggestion found */

/*	int k = bm_n;
	while (--k >= 0) {
		if (!bookmarks[k].name)
			continue;
		if (strncmp(last_word, bookmarks[k].name, strlen(last_word)) == 0) {
			if (!bookmarks[k].path)
				continue;
			msg_area = 1;
			rl_save_prompt();
			rl_message("\001\x1b[0;36m(%s):\002\x1b[0m ", bookmarks[k].path);
			break;
		}
	}

	if (k < 0 && msg_area == 1) {
		rl_restore_prompt();
		rl_clear_message();
	} */

	/* Clear current suggestion, if any, only if no escape char is contained
	 * in the current input sequence. This is mainly to avoid erasing
	 * the suggestion if moving thought the text via the arrow keys */
	if (suggestion.printed) {
		if (!strchr(last_word, '\x1b')) {
			clear_suggestion();
			free(full_line);
			goto FAIL;
		} else {
			/* Go to SUCCESS to avoid removing the suggestion buffer */
			printed = 1;
			goto SUCCESS;
		}
	}

SUCCESS:
	free(full_line);
	if (printed) {
		suggestion.printed = 1;
		/* Restore color */
		fputs("\x1b[0m", stdout);
		if (!cur_color)
			fputs(df_c, stdout);
		else
			fputs(cur_color, stdout);
	} else {
		suggestion.printed = 0;
	}
	free(last_word);
	if (inserted_c)
		return (-1);
	return EXIT_SUCCESS;

FAIL:
	suggestion.printed = 0;
	free(last_word);
	free(suggestion_buf);
	suggestion_buf = (char *)NULL;
	if (inserted_c)
		return (-1);
	return EXIT_FAILURE;
}

#endif /* !_NO_SUGGESTIONS */
