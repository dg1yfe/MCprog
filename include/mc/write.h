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
/* Writing a codeplug to a radio -- spec.md section 8 (W-n).
 *
 * The protocol layer already knows how to write; this is the part that decides whether it should.
 * A half-written EEPROM is how a working radio becomes a brick, and the radio cannot be recovered
 * without another programmer, so every gate here fails closed.
 */
#ifndef MC_WRITE_H
#define MC_WRITE_H

#include "mc/codeplug.h"
#include "mc/protocol.h"

typedef struct {
	char backup[128];   /* where the pre-write copy went; empty if it never got that far */
	/* Big enough to hold a session error (160) and a verification error (200) alongside the backup
	 * path, because a failed mid-write is exactly when none of the three may be truncated. */
	char err[640];      /* why it stopped, when it stopped */
	int records;        /* records written and verified */
	int changed;        /* bytes that differ from what the radio held */
	size_t radio_len;   /* what the pre-write read returned */
	int counter;      /* W-5: 1 if the counter was bumped for this write, 0 if the model has none */
} mc_write_report;

/* Compare `img` against what the radio holds and report how many bytes differ, or -1 with `why`
 * set if a difference is one MCprog cannot account for (K-30).  Exposed separately so a caller can
 * show the user what a write would do before asking them to confirm it. */
int mc_write_explain(const mc_image *img, const uint8_t *radio, size_t radio_len,
                     char *why, size_t whysz);

/* Does a record read back from the radio differ from what was written in a way that matters?
 * Returns 0 if it is acceptable, -1 with `err` set otherwise.  `p` is the band's P constant.
 *
 * K-11: a frequency has more than one legal spelling, so the three bytes of a frequency field are
 * compared by the frequency they decode to.  Everything else is compared byte for byte.  Exposed
 * because it is the rule, not an implementation detail -- and because a rule this easy to get
 * backwards should be tested directly rather than only through a write.
 */
int mc_write_verify_record(const mc_image *img, unsigned p, uint16_t addr, const uint8_t *want,
                           const uint8_t *got, char *err, size_t errsz);

/* Read the radio, back it up, check the gates, write, and verify every record.
 *
 * W-1 is the caller's: this function does not check any flag, so do not call it unless the user
 * has asked for a write explicitly.  Everything else is here:
 *
 *   W-2  a full read is written to a timestamped file before the first write byte; if either the
 *        read or the backup fails, nothing is written
 *   W-3  fatal gates -- checksum valid, band programmed, size matches the radio, and every
 *        difference from the radio's copy accountable (K-30)
 *   W-4  each record is read back and compared; frequency fields compare by decoded frequency,
 *        never by bytes (K-11)
 *   W-6  records in order 0..N, as the original writes them
 *
 *   W-5  the write counter is bumped in the bytes sent to the radio, and the checksum
 *        recomputed -- the caller's image is not touched, so a file save never advances it.
 *        Models with no measured counter are sent unchanged; `rep->counter` says which happened
 *
 * Returns 0 on success, -1 with rep->err set otherwise.
 */
int mc_write_radio(mc_session *s, const mc_image *img, mc_write_report *rep);

#endif /* MC_WRITE_H */
