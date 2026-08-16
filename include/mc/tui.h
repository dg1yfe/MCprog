/*
 *   MCprog - A programmer for the Motorola MC micro radio family,
 *            replacing the 1987 Radio Service Software
 *
 *   Copyright (C) 2026  Felix Erckenbrecht, DG1YFE
 *
 *    This file is part of MCprog.
 *
 *    MCprog is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    MCprog is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with MCprog.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MC_TUI_H
#define MC_TUI_H

#include "mc/codeplug.h"

/* Run the editor over `image` until the user quits.
 *
 * `filepath` is where `s` saves; pass NULL or "" for an image read from a radio, and the user is
 * asked for a name the first time they save.  `note` is shown once on the status line, which is how
 * model detection reports what it found.
 */
int mc_tui_run(mc_image *image, const char *filepath, const char *note);

#endif /* MC_TUI_H */
