/* history.h */

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

#ifndef HISTORY_H
#define HISTORY_H

int save_dirhist(void);
void add_to_cmdhist(const char *cmd);
int record_cmd(char *input);
int get_history(void);
int history_function(char **comm);
int run_history_cmd(const char *cmd);
void add_to_dirhist(const char *dir_path);
int log_function(char **comm);
void log_msg(char *_msg, int print);

#endif /* HISTORY_H */
