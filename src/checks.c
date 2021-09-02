/* checks.c -- misc check functions */

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

#include "helpers.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef __NetBSD__
# include <sys/param.h>
# if __NetBSD_Prereq__(9,99,63)
#  include <sys/acl.h>
#  define _ACL_OK
# endif /* __NetBSD_Prereq__ */
#elif !defined(__HAIKU__) && !defined(__OpenBSD__)
# include <sys/acl.h>
# define _ACL_OK
#endif /* __NetBSD__ */

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aux.h"
#include "misc.h"

/* Terminals known not to be able to handle escape sequences */
static const char *UNSUPPORTED_TERM[] = {"dumb", /*"cons25",*/ "emacs", NULL};

void
check_term(void)
{
	char *_term = getenv("TERM");
	if (!_term) {
		fprintf(stderr, _("%s: Error getting terminal type\n"), PROGRAM_NAME);
		exit(EXIT_FAILURE);
	}

	int i;
	for (i = 0; UNSUPPORTED_TERM[i]; i++) {
		if (*_term == *UNSUPPORTED_TERM[i]
		&& strcmp(_term, UNSUPPORTED_TERM[i]) == 0) {
			fprintf(stderr, _("%s: '%s': Unsupported terminal. This "
					"terminal cannot understand escape sequences\n"),
					PROGRAM_NAME, UNSUPPORTED_TERM[i]);
			exit(EXIT_FAILURE);
		}
	}

	return;
}

/* Return 1 if current user has access to FILE. Otherwise, return zero */
int
check_file_access(struct fileinfo file)
{
	int f = 0; /* Hold file ownership flags */

	mode_t val = (file.mode & (mode_t)~S_IFMT);
	if (val & S_IRUSR) f |= R_USR;
	if (val & S_IXUSR) f |= X_USR;

	if (val & S_IRGRP) f |= R_GRP;
	if (val & S_IXGRP) f |= X_GRP;

	if (val & S_IROTH) f |= R_OTH;
	if (val & S_IXOTH) f |= X_OTH;

	if (file.dir) {
		if ((f & R_USR) && (f & X_USR) && file.uid == user.uid)
			return 1;
		if ((f & R_GRP) && (f & X_GRP) && file.gid == user.gid)
			return 1;
		if ((f & R_OTH) && (f & X_OTH))
			return 1;
	} else {
		if ((f & R_USR) && file.uid == user.uid)
			return 1;
		if ((f & R_GRP) && file.gid == user.gid)
			return 1;
		if (f & R_OTH)
			return 1;
	}

	return 0;
}

char *
get_sudo_path(void)
{
	char *p = getenv("CLIFM_SUDO_CMD");
	char *sudo = get_cmd_path(p ? p : DEF_SUDO_CMD);

	if (!sudo) {
		fprintf(stderr, _("%s: %s: No such file or directory\n"),
				PROGRAM_NAME, p ? p : DEF_SUDO_CMD);
		return (char *)NULL;
	}

	return sudo;
}

/* Check a file's immutable bit. Returns 1 if true, zero if false, and
 * -1 in case of error */
int
check_immutable_bit(char *file)
{
#if !defined(FS_IOC_GETFLAGS) || !defined(FS_IMMUTABLE_FL)
	UNUSED(file);
	return 0;
#else
	int attr, fd, immut_flag = -1;

	fd = open(file, O_RDONLY);
	if (fd == -1) {
		fprintf(stderr, "'%s': %s\n", file, strerror(errno));
		return -1;
	}

	ioctl(fd, FS_IOC_GETFLAGS, &attr);
	if (attr & FS_IMMUTABLE_FL)
		immut_flag = 1;
	else
		immut_flag = 0;

	close(fd);

	if (immut_flag)
		return 1;

	return 0;
#endif /* !defined(FS_IOC_GETFLAGS) || !defined(FS_IMMUTABLE_FL) */
}

/* Return 1 if FILE has some ACL property and zero if none
 * See: https://man7.org/tlpi/code/online/diff/acl/acl_view.c.html */
int
is_acl(char *file)
{
#ifndef _ACL_OK
	UNUSED(file);
	return 0;
#else
	if (!file || !*file)
		return 0;

	acl_t acl;
	acl = acl_get_file(file, ACL_TYPE_ACCESS);

	if (!acl)
		return 0;

	acl_entry_t entry;
	int entryid, num = 0;

	for (entryid = ACL_FIRST_ENTRY;; entryid = ACL_NEXT_ENTRY) {
		if (acl_get_entry(acl, entryid, &entry) != 1)
			break;
		num++;
	}

	acl_free(acl);

	/* If num > 3 we have something else besides owner, group, and others,
	 * that is, we have at least one ACL property */
	if (num > 3)
		return 1;

	return 0;
#endif /* _ACL_OK */
}

/* Check whether a given string contains only digits. Returns 1 if true
 * and 0 if false. It does not work with negative numbers */
int
is_number(const char *restrict str)
{
	for (; *str; str++)
		if (*str > '9' || *str < '0')
			return 0;

	return 1;
}

/* Check CMD against a list of internal commands */
int
is_internal_c(const char *restrict cmd)
{
	const char *int_cmds[] = {
	    "?", "help",
	    "ac", "ad",
	    "acd", "autocd",
	    "actions",
	    "alias",
	    "ao", "auto-open",
	    "b", "back",
	    "bh", "fh",
	    "bm", "bookmarks",
	    "br", "bulk",
	    "c", "cp",
	    "cc", "colors",
	    "cd",
	    "cl", "columns",
	    "cmd", "commands",
	    "cs", "colorschemes",
		"d", "dup",
	    "ds", "desel",
	    "edit",
	    "exp", "export",
	    "ext",
	    "f", "forth",
	    "fc",
	    "ff", "folders-first",
	    "fs",
	    "ft", "filter",
	    "history",
	    "hf", "hidden",
	    "icons",
	    "jump", "je", "jc", "jp", "jo",
	    "kb", "keybinds",
	    "l", "ln", "le",
	    "lm",
	    "log",
	    "m", "mv",
	    "md", "mkdir",
	    "mf",
	    "mm", "mime",
	    "mp", "mountpoints",
	    "msg", "messages",
	    "n", "new",
	    "net",
	    "o", "open",
	    "opener",
	    "p", "pp", "pr", "prop",
	    "path", "cwd",
		"paste",
	    "pf", "prof", "profile",
	    "pg", "pager",
	    "pin", "unpin",
		"quit",
	    "r", "rm",
	    "rf", "refresh",
	    "rl", "reload",
	    "s", "sel",
	    "sb", "selbox",
	    "shell",
	    "splash",
	    "st", "sort",
	    "t", "tr", "trash",
	    "te",
	    "tips",
	    "touch",
	    "u", "undel", "untrash",
	    "uc", "unicode",
	    "unlink",
		"v",
	    "ver", "version",
	    "ws",
	    "x", "X",
	    NULL};

	int found = 0;
	int i = (int)(sizeof(int_cmds) / sizeof(char *)) - 1;

	while (--i >= 0) {
		if (*cmd == *int_cmds[i] && strcmp(cmd, int_cmds[i]) == 0) {
			found = 1;
			break;
		}
	}

	if (found)
		return 1;

	/* Check for the search and history functions as well */
	else if ((*cmd == '/' && access(cmd, F_OK) != 0) || (*cmd == '!'
	&& (_ISDIGIT(cmd[1]) || (cmd[1] == '-' && _ISDIGIT(cmd[2]))
	|| cmd[1] == '!')))
		return 1;

	return 0;
}

/* Check cmd against a list of internal commands. Used by parse_input_str()
 * to know if it should perform additional expansions, like glob, regex,
 * tilde, and so on. Only internal commands dealing with file names
 * should be checked here */
int
is_internal(const char *cmd)
{
	const char *int_cmds[] = {
	    "cd",
	    "o", "open",
	    "s", "sel",
	    "p", "pr", "prop",
	    "r",
	    "t", "tr", "trash",
	    "mm", "mime",
	    "n", "new",
	    "bm", "bookmarks",
	    "br", "bulk",
	    "ac", "ad",
	    "exp", "export",
	    "pin",
	    "jc", "jp",
	    "bl", "le",
	    "te",
	    NULL};

	int found = 0;
	int i = (int)(sizeof(int_cmds) / sizeof(char *)) - 1;

	while (--i >= 0) {
		if (*cmd == *int_cmds[i] && strcmp(cmd, int_cmds[i]) == 0) {
			found = 1;
			break;
		}
	}

	if (found)
		return 1;

	/* Check for the search function as well */
	else if (*cmd == '/' && access(cmd, F_OK) != 0)
		return 1;

	return 0;
}

/* Return one if STR is a command in PATH or zero if not */
int
is_bin_cmd(const char *str)
{
	char *p = (char *)str, *q = (char *)str;
	int index = 0, space_index = -1;

	while (*p) {
		if (*p == ' ') {
			*p = '\0';
			space_index = index;
			break;
		}
		p++;
		index++;
	}

	size_t i;
	for (i = 0; bin_commands[i]; i++) {
		if (*q == *bin_commands[i] && q[1] == bin_commands[i][1] && strcmp(q, bin_commands[i]) == 0) {
			if (space_index != -1)
				q[space_index] = ' ';
			return 1;
		}
	}

	if (space_index != -1)
		q[space_index] = ' ';

	return 0;
}

/* Returns 0 if digit is found and preceded by a letter in STR, or one if not */
int
digit_found(const char *str)
{
	char *p = (char *)str;
	int c = 0;

	while (*p) {
		if (c++ && _ISDIGIT(*p) && _ISALPHA(*(p - 1)))
			return 1;
		p++;
	}

	return 0;
}

/* Check if the 'file' command is available: it is needed by the mime
 * function */
/*
void
file_cmd_check(void)
{
	file_cmd_path = get_cmd_path("file");

	if (!file_cmd_path) {
		flags &= ~FILE_CMD_OK;
		_err('n', PRINT_PROMPT, _("%s: 'file' command not found. "
				  "Specify an application when opening files. Ex: 'o 12 nano' "
				  "or just 'nano 12'\n"), PROGRAM_NAME);
	}

	else
		flags |= FILE_CMD_OK;
} */

int
check_regex(char *str)
{
	if (!str || !*str)
		return EXIT_FAILURE;

	int char_found = 0;
	char *p = str;

	while (*p) {
		/* If STR contains at least one of the following chars */
		if (*p == '*' || *p == '?' || *p == '[' || *p == '{' || *p == '^'
		|| *p == '.' || *p == '|' || *p == '+' || *p == '$') {
			char_found = 1;
			break;
		}

		p++;
	}

	/* And if STR is not a file name, take it as a possible regex */
	if (char_found)
		if (access(str, F_OK) == -1)
			return EXIT_SUCCESS;

	return EXIT_FAILURE;
}

/* Returns the parsed aliased command in an array of strings, if
 * matching alias is found, or NULL if not. */
char **
check_for_alias(char **args)
{
	if (!aliases_n || !aliases || !args)
		return (char **)NULL;

	char *aliased_cmd = (char *)NULL;
	size_t cmd_len = strlen(args[0]);
	char tmp_cmd[PATH_MAX * 2 + 1];
	snprintf(tmp_cmd, sizeof(tmp_cmd), "%s=", args[0]);

	register int i = (int)aliases_n;
	while (--i >= 0) {

		if (!aliases[i])
			continue;
		/* Look for this string: "command=", in the aliases file */
		/* If a match is found */

		if (*aliases[i] != *args[0] ||
		    strncmp(tmp_cmd, aliases[i], cmd_len + 1) != 0)
			continue;

		/* Get the aliased command */
		aliased_cmd = strbtw(aliases[i], '\'', '\'');

		if (!aliased_cmd)
			return (char **)NULL;

		if (!*aliased_cmd) { /* zero length */
			free(aliased_cmd);
			return (char **)NULL;
		}

		args_n = 0; /* Reset args_n to be used by parse_input_str() */

		/* Parse the aliased cmd */
		char **alias_comm = parse_input_str(aliased_cmd);
		free(aliased_cmd);

		if (!alias_comm) {
			args_n = 0;
			fprintf(stderr, _("%s: Error parsing aliased command\n"), PROGRAM_NAME);
			return (char **)NULL;
		}

		register size_t j;

		/* Add input parameters, if any, to alias_comm */
		if (args[1]) {
			for (j = 1; args[j]; j++) {
				alias_comm = (char **)xrealloc(alias_comm,
				    (++args_n + 2) * sizeof(char *));
				alias_comm[args_n] = savestring(args[j],
				    strlen(args[j]));
			}
		}

		/* Add a terminating NULL string */
		alias_comm[args_n + 1] = (char *)NULL;

		/* Free original command */
		for (j = 0; args[j]; j++)
			free(args[j]);
		free(args);

		return alias_comm;
	}

	return (char **)NULL;
}

/* Keep only the last MAX records in LOGFILE */
void
check_file_size(char *logfile, int max)
{
	if (!config_ok)
		return;

	/* Create the file, if it doesn't exist */
	FILE *log_fp = (FILE *)NULL;
	struct stat file_attrib;

	if (stat(logfile, &file_attrib) == -1) {
		log_fp = fopen(logfile, "w");

		if (!log_fp) {
			_err(0, NOPRINT_PROMPT, "%s: '%s': %s\n", PROGRAM_NAME,
			    logfile, strerror(errno));
		} else
			fclose(log_fp);

		return; /* Return anyway, for, being a new empty file, there's
		no need to truncate it */
	}

	/* Once we know the files exists, keep only max logs */
	log_fp = fopen(logfile, "r");

	if (!log_fp) {
		_err(0, NOPRINT_PROMPT, "%s: log: %s: %s\n", PROGRAM_NAME,
		    logfile, strerror(errno));
		return;
	}

	int logs_num = 0, c;

	/* Count newline chars to get amount of lines in the log file */
	while ((c = fgetc(log_fp)) != EOF) {
		if (c == '\n')
			logs_num++;
	}

	if (logs_num <= max) {
		fclose(log_fp);
		return;
	}

	/* Set the file pointer to the beginning of the log file */
	fseek(log_fp, 0, SEEK_SET);

	/* Create a temp file to store only newest logs */
	char *rand_ext = gen_rand_str(6);

	if (!rand_ext) {
		fclose(log_fp);
		return;
	}

	char *tmp_file = (char *)xnmalloc(strlen(config_dir) + 12, sizeof(char));
	sprintf(tmp_file, "%s/log.%s", config_dir, rand_ext);
	free(rand_ext);

	FILE *log_fp_tmp = fopen(tmp_file, "w+");

	if (!log_fp_tmp) {
		fprintf(stderr, "log: %s: %s", tmp_file, strerror(errno));
		fclose(log_fp);
		free(tmp_file);
		return;
	}

	int i = 1;
	size_t line_size = 0;
	char *line_buff = (char *)NULL;

	while (getline(&line_buff, &line_size, log_fp) > 0) {
		/* Delete old entries = copy only new ones */
		if (i++ >= logs_num - (max - 1))
			fprintf(log_fp_tmp, "%s", line_buff);
	}

	free(line_buff);
	fclose(log_fp_tmp);
	fclose(log_fp);
	unlink(logfile);
	rename(tmp_file, logfile);
	free(tmp_file);

	return;
}
