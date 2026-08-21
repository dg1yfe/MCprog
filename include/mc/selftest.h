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
 * This exists to answer, in one run, the questions that only hardware can settle, and to produce a
 * report someone can paste into an issue.  It is one .c, one .h, and one `--selftest` branch in
 * main.c, deliberately touching nothing else, so that deleting it stays a clean subtraction
 * whenever it has served its purpose.
 *
 * The first radio through it -- an EZA 9, 17 Aug 2026 -- corrected four things (see spec.md P-20,
 * P-22, P-23, P-24) and found two defects in this file.  Radios are scarce enough that it earns its
 * place until several more have been through it.
 *
 * What it is for, in order of how much is riding on it:
 *
 *   1. P-11/P-12, the control lines.  MCprog leaves DTR de-asserted and asserts RTS.  The first
 *      radio answered on that combination and on DTR asserted, and was silent whenever RTS was
 *      de-asserted -- so RTS is what selects programming mode, and it has never come back within a
 *      session once dropped.
 *      The selftest therefore tries combinations in turn and STOPS at the first that answers,
 *      keeping that port open for the rest of the run.  If the working combination is not the one
 *      MCprog uses by default, everything else would fail; this says so in ten seconds rather than
 *      costing an evening.
 *   2. P-24, which of the two end-of-memory NAK forms this radio uses -- both are real: the EVA
 *      captures echo the header first, the EZA 9 NAKs bare.
 *   3. P-20, the ident: its length is per-model, and whether it is answered more than once.
 *   4. `)02`, measured at last on the EZA 9 as a plain two-byte read.
 *   5. P-25's burn delay and P-30's byte timeout, measured rather than assumed.
 *
 * It should need nobody watching it: with no `port` set it goes looking for the radio, and when a
 * person really is unavoidable -- no cable, or a radio that only a power cycle will revive -- it
 * says so in a block that cannot be missed, then waits for the radio to return.
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
