/* colors.h */

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

#ifndef COLORS_H
#define COLORS_H

#include <sys/stat.h>

int set_colors(const char *colorscheme, int env);
void color_codes(void);
size_t get_colorschemes(void);
int cschemes_function(char **args);
void colors_list(char *ent, const int i, const int pad, const int new_line);
char *get_ext_color(const char *ext);
char *get_dir_color(const char *filename, const mode_t mode);
char *get_file_color(const char *filename, const struct stat attr);

#endif /* COLORS_H */
