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
/* First contact with a real radio -- M6.
 *
 * TEMPORARY.  This exists to answer, in one run, the questions that only hardware can settle, and
 * to produce a report someone can paste into an issue.  Once the answers are in spec.md it should
 * be deleted: it is one .c, one .h, and one `--selftest` branch in main.c, deliberately touching
 * nothing else so that removing it is a clean subtraction.
 *
 * What it is for, in order of how much is riding on it:
 *
 *   1. P-11/P-12, the control lines.  MCprog leaves DTR de-asserted and asserts RTS, on the reading
 *      that a de-asserted line sits at its negative level and that is what the interface's level
 *      shifter draws from, while RTS drives the radio's HUB/PGM input.  Nothing has ever tested it.
 *      The selftest tries all four combinations and reports which ones the radio answers.  If the
 *      answer is not the one MCprog uses, everything else fails and this says why in ten seconds
 *      instead of an evening.
 *   2. P-24, which of the two end-of-memory NAK forms this radio uses.
 *   3. P-20, whether the ident really is answered more than once per power-up.
 *   4. `)02`, which appears in neither capture and whose reply is therefore unknown.
 *   5. P-25's burn delay and P-30's byte timeout, measured rather than assumed.
 *
 * Read-only unless `write_back` is set, and even then the only thing written is the codeplug the
 * radio already holds -- byte for byte, after a backup -- so the write path is exercised without
 * changing what the radio does.
 */
#ifndef MC_SELFTEST_H
#define MC_SELFTEST_H

#include <stdio.h>
#include "mc/codeplug.h"
#include "mc/serial.h"

typedef struct {
	const char *port;
	const mc_serial_opts *opts; /* baud and parity; the line handling is the selftest's own */
	const char *report_path;    /* markdown report */
	const char *trace_path;     /* wire log, in the format the conformance suite reads */
	const char *codeplug_path;  /* where to save what was read */
	int write_back;             /* also write the radio's own bytes back, to exercise P-25/P-42 */
	int probe_lines;            /* try all four DTR/RTS combinations (P-11) */
} mc_selftest_opts;

/* Runs the battery, writes the report, and returns 0 if the radio was reached at all.  A failing
 * probe is data, not an error: it goes in the report and the run continues. */
int mc_selftest(const mc_selftest_opts *o);

#endif /* MC_SELFTEST_H */
