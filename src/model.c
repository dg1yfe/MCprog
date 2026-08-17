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
/* Model descriptors -- spec.md K-20.
 *
 * The names match testdata/codeplug/[*].vec.  MCEZ13 is listed by the original software
 * as 128/256/512/1024 bytes; only the 128-byte variant has a measured map, so that is what is
 * described here and the size is checked rather than assumed.
 */
#include <stdio.h>
#include <string.h>
#include "mc/codeplug.h"

/* Channel flag bits, spec K-22.  Every 'C' entry was pinned by driving the 1987 editor and
 * diffing what it wrote; see doc/EEPROM_MAP_EV9.md and doc/EEPROM_MAP_EZA.md. */
static const mc_flag FLAGS_EVA[] = {
	{ "clock_shift", 3, MC_HALF_BOTH, 0, 'C' },
	{ "decode",      4, MC_HALF_BOTH, 0, 'C' },
	{ "tx_inhibit",  5, MC_HALF_BOTH, 0, 'C' },
	{ "encode",      6, MC_HALF_BOTH, 0, 'C' },
	{ "power_high",  7, MC_HALF_BOTH, 0, 'C' },
};
static const mc_flag FLAGS_EZA9[] = {
	{ "auto_ack",    3, MC_HALF_BOTH, 0, 'C' },
	{ "decode",      4, MC_HALF_BOTH, 0, 'C' },
	{ "tx_inhibit",  5, MC_HALF_BOTH, 0, 'C' },
	{ "encode",      6, MC_HALF_BOTH, 0, 'C' },
	{ "clock_shift", 7, MC_HALF_RX,   0, 'C' },
	/* Real and preserved across a round trip, but these builds never expose it; read as RF power
	 * from the disassembly and unrefuted, not measured. */
	{ "power_high",  7, MC_HALF_TX,   0, 'S' },
};
/* MCEZ13 keeps PL in tables and TX inhibit in a global bit, so the record carries almost nothing.
 * Its clock shift is stored inverted: the bit is SET when the screen shows N. */
static const mc_flag FLAGS_EZ13[] = {
	{ "clock_shift",  6, MC_HALF_TX, 1, 'C' },
	{ "reserved_b7",  7, MC_HALF_TX, 0, 'S' },
};

#define NF(a) (a), (uint8_t)(sizeof(a) / sizeof((a)[0]))

/* PL layout, measured per model (K-14).  MCEZ13 is deliberately left at zero: its decoder and
 * encoder tables are known but the per-channel indexing is not, and its read is still blocked, so
 * the tool exposes nothing rather than guessing. */
static const mc_model MODELS[] = {
	/* name       size cksum cklen  chan   band  refdiv nch str tx rx num  flags          PL: tone  list count mode   dec  max */
	{ "eva_56",    512, 0x000, 0,    0x0E0, 0x0DC, 0x0D4, 32, 8, 2, 5, 1, NF(FLAGS_EVA),  0x047, 0x047, 0x0CE, 0x1FD, 0,     10 },
	{ "eva_sel5",  512, 0x000, 0,    0x0E0, 0x0DC, 0x0D4, 32, 8, 2, 5, 1, NF(FLAGS_EVA),  0x047, 0x047, 0x0CE, 0x1FD, 0,     10 },
	{ "eza_sel5",  256, 0x000, 0,    0x0C8, 0x082, 0x0C4,  8, 6, 0, 3, 0, NF(FLAGS_EZA9), 0x02F, 0x031, 0x083, 0x07F, 0,     10 },
	/* MCEZ13: no mode byte -- an encoder table and a decoder table -- and its checksum covers 126
	 * of its 128 bytes, measured off the sum loop at CS:0x767E. */
	{ "eza_cspl",  128, 0x001, 126,  0x039, 0x037, 0x002,  8, 6, 0, 3, 0, NF(FLAGS_EZ13), 0x022, 0x022, 0,     0,     0x00E, 10 },
};
static const size_t NMODELS = sizeof MODELS / sizeof MODELS[0];

const mc_model *mc_model_by_name(const char *name)
{
	size_t i;
	for (i = 0; i < NMODELS; i++)
		if (strcmp(MODELS[i].name, name) == 0)
			return &MODELS[i];
	return NULL;
}

const mc_model *mc_model_by_index(size_t i)
{
	return i < NMODELS ? &MODELS[i] : NULL;
}

const mc_flag *mc_flags(const mc_model *m, size_t *n)
{
	if (n)
		*n = m->nflags;
	return m->flags;
}

const mc_flag *mc_flag_by_name(const mc_model *m, const char *name)
{
	size_t i;
	for (i = 0; i < m->nflags; i++)
		if (strcmp(m->flags[i].name, name) == 0)
			return &m->flags[i];
	return NULL;
}

const mc_model *mc_model_detect(const uint8_t *bytes, size_t len, char *note, size_t notesz)
{
	const mc_model *m, *best = NULL;
	size_t i, n = 0;
	char list[128] = "";

	for (i = 0; (m = mc_model_by_index(i)) != NULL; i++) {
		mc_image t;
		t.model = m;
		t.bytes = (uint8_t *)bytes;
		t.len = len;
		if (m->size != len || !mc_checksum_valid(&t))
			continue;
		if (!best)
			best = m;
		n++;
		snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", n > 1 ? ", " : "",
		         m->name);
	}
	if (!best)
		snprintf(note, notesz, "no model matches %u bytes with a valid checksum", (unsigned)len);
	else if (n > 1)
		snprintf(note, notesz, "%u models fit (%s); assuming %s -- use --model to choose",
		         (unsigned)n, list, best->name);
	else
		snprintf(note, notesz, "detected %s", best->name);
	return best;
}
