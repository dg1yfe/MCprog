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
	/* Was 'S': "these builds never expose it".  The EDITOR does not, but the Master build's report
	 * generator does -- its channel table has a POWER column.  Planting bit 7 of the TX half with
	 * the hidden EEPROM monitor and printing flips it HIGH -> LOW, so the bit, the half and the
	 * sense are all measured now (`tools/revoracle.py --build ez9 --bits').  Note the sense is the
	 * opposite of MCEZ13's, which is why that one carries inverted=1 and this one does not. */
	{ "power_high",  7, MC_HALF_TX,   0, 'C' },
};
/* MCEZ13 keeps PL in tables and TX inhibit in a global bit, so the record carries almost nothing.
 * BOTH its flags are stored inverted: the bit is SET when the report shows N / LOW.
 *
 * Bit 7 was carried as `reserved_b7' -- "a stored bit the original never exposes" -- because the
 * write-back oracle sees nothing when MAB 889 RF POWER LEVEL is edited.  It is the power level.
 * The editor reads the bit and never writes it, which is invisible to a field-to-bytes oracle;
 * planting the bit with the RSS's own EEPROM monitor and reading its printout shows it plainly
 * (`tools/revoracle.py'):  bit 7 clear prints HIGH, set prints LOW, on every channel. */
static const mc_flag FLAGS_EZ13[] = {
	{ "clock_shift",  6, MC_HALF_TX, 1, 'C' },
	{ "power_high",   7, MC_HALF_TX, 1, 'C' },
};

#define NF(a) .flags = (a), .nflags = (uint8_t)(sizeof(a) / sizeof((a)[0]))

/* PL layout (K-14) and the auto-acknowledge delay (K-15), measured per model.  AAK is EZA 9 only:
 * no other model's map has been shown to carry it, and an offset nobody has measured is worse than
 * no offset at all.  MCEZ13 is deliberately left at zero: its decoder and
 * encoder tables are known but the per-channel indexing is not, and its read is still blocked, so
 * the tool exposes nothing rather than guessing. */

/* K-16.  Read out of the original's own four parallel word arrays -- `MCEV_56' 0xC9B3, `MCEV9'
 * 0x8C5D, identical byte for byte in every EVA build, which is why both 512-byte models carry it.
 * The original's table indexes one byte low for the byte fields (it always fetches a word and
 * masks); these offsets are the field itself.  `den' is its A, `add_ms' its B in milliseconds. */
static const mc_timer TIMERS_EVA[] = {
	/*  name                  off    mask     w  den  add_ms */
	{ "RX/TX delay",        0x0B3, 0x00FF,   1,   1,     0 },
	{ "encoder pretime",    0x0B4, 0x00FF,   1,   1,     0 },
	{ "encoder hold time",  0x0B5, 0x00FF,   1,   1,     0 },
	{ "intersequence",      0x0B6, 0x00FF,   1,   1,     0 },
	{ "synth lock time",    0x0B7, 0x00FF,   1,  12,    10 },
	{ "TX time-out",        0x0B8, 0x7FFF,   2,   1,  4000 },
	{ "rekey delay",        0x0BA, 0x7FFF,   2,   1,     0 },
	{ "auto reset time",    0x0BC, 0x1FFF,   2,   1,     0 },
	{ "ext alarm time",     0x0BE, 0x3FFF,   2,   1,     0 },
	{ "emergency RX time",  0x0C0, 0x7FFF,   2,   1,     0 },
	{ "emergency TX time",  0x0C2, 0x7FFF,   2,   1,     0 },
	{ "emergency debounce", 0x0C4, 0x007F,   1,   1,     0 },
};

#define NT(x) .timers = (x), .ntimers = (uint8_t)(sizeof (x) / sizeof (x)[0])

/* Order matters: mc_model_detect() takes the FIRST entry that fits, so this is the preference order
 * when more than one model matches.
 *
 * The two 512-byte models are the SAME hardware -- EVA 9 -- differing only in which signalling the
 * set is fitted with, and nothing in a 512-byte image distinguishes them: same size, same checksum
 * rule, same channel layout.  `eva_sel5' leading is a **preference, not a determination**, and the
 * note says as much so nobody mistakes it for detection.
 *
 * When the image came from a radio there is no need to guess: the ident names the signalling.  See
 * mc_model_detect_ident().
 *
 * (For the avoidance of doubt: `eva_56' is not a trunking radio.  Trunking is the EVA 5 / RN43P
 * MPT1327 system -- different hardware, different reference crystal, its own 1998 software.) */
/* Designated initialisers on purpose.  These entries were positional, and adding a field in the
 * middle of the struct then shifted every value after it silently -- the compiler cannot catch it
 * because the types line up.  Name each field and that whole class of bug goes away. */
/* Note .pl_ch_enc/.pl_ch_dec: 0 is a REAL record offset (the M110 keeps its encode word at +0), so
 * every model that does not hold PL in the channel record must say MC_PL_NONE explicitly.  A
 * designated initialiser would otherwise default them to 0 and give every MC micro channel a
 * phantom PL field over its first two bytes. */
#define MC_MICRO .cksum_target = 0xFF, .band_shift = 4, .band_mask = 7, \
                 .pl_ch_enc = MC_PL_NONE, .pl_ch_dec = MC_PL_NONE

/* M110 band -> frequency multiplier P, indexed by bits 0-3 of 0x0A.
 *
 * Three entries are measured, each on a real radio and confirmed by the RSS's own `Radio Type:'
 * line and by every programmed frequency decoding exactly:
 *
 *     7  VHI  P = 80    run4 (0x0A = 0xF7, 144.8000) and write-runs/report8 (0x37, 144.8000)
 *    12  ULO  P = 254   run3 (0x0A = 0x3C, 431.0125 / 438.6125)
 *    15  UHI  P = 254   run5 (0x0A = 0x2F, 439.9875)
 *
 * The rest are ZERO because they have never been measured -- not because P is zero.  Zero reads
 * downstream as "not computable", which is the honest answer and is exactly how the MC micro's
 * unprogrammed band 7 already behaves.  VLO, MIB, UX1 and UX2 are the known gaps.
 *
 * `doc/M110_MNEMONICS.md' had this field as bits 0-2; its five oracle samples were all VLO(4) and
 * VHI(7), which have bit 3 clear.  ULO(12) and UHI(15) need the fourth bit. */
static const uint8_t BAND_P_M110[16] = {
	/*  0 */ 0,   /*  1 */ 0,   /*  2 */ 0,   /*  3 */ 0,
	/*  4 */ 0,   /*  5 */ 0,   /*  6 */ 0,   /*  7 */ 80,   /* VHI */
	/*  8 */ 0,   /*  9 */ 0,   /* 10 */ 0,   /* 11 */ 0,
	/* 12 */ 254, /* ULO */     /* 13 */ 0,   /* 14 */ 0,    /* 15 */ 254, /* UHI */
};

/* Channel slots.  Both families zero-fill unused records rather than terminating with 0xFF, so the
 * count cannot be read off an image -- it is the size of the region the table occupies.
 *
 * CSQ/PL: 10-byte records from 0x1B; the codeplug ends at 0x7F, giving 10 slots.  The stride is
 * measured directly: both hardware radios program two channels, at 0x1B and 0x25.
 * Sel 5:  12-byte records from 0x92; the device ends at 0x100, giving 9 slots.
 *
 * The Sel 5 stride is NOT what a single record suggests.  Both hardware radios program one channel
 * and leave exactly 7 bytes non-zero (0x92..0x98), which reads as a 7-byte record -- and is wrong.
 * Driving the 1989 RSS with a three-channel codeplug puts them at 0x92, 0x9E and 0xAA, so the
 * record is 12 bytes with the last five unused in these codeplugs.  All three frequencies decode
 * exactly as programmed (163.1100 / 150.0000 / 170.0000).  A stride taken from one record would
 * have put channel 2 five bytes early.
 *
 * That the whole region is a channel table remains INFERENCE from radios that leave the rest zero,
 * not a measurement -- see spec.md K-25. */
#define M110_CSPL_NCHAN 10
#define M110_SEL5_NCHAN 9

static const mc_model MODELS[] = {
	{ .name = "eva_sel5", .size = 512, MC_MICRO,
	  .cksum = 0x000, .chan = 0x0E0, .band = 0x0DC, .refdiv = 0x0D4,
	  .nchan = 32, .stride = 8, .tx = 2, .rx = 5, .numbered = 1, NF(FLAGS_EVA),
	  .pl_tone = 0x047, .pl_list = 0x047, .pl_count = 0x0CE, .pl_mode = 0x1FD,
	  .trak = 1, .trak_ct = 0x0DA, .size_flag = 0x0CF,
	  .pl_max = 10, .pl_k = MC_PL_K_EVA, NT(TIMERS_EVA), .wcount = 0x0AF, .wcount_clr = 0x00,
	  .about = "EVA, SEL5 signalling -- MCEV9, MCEV9M" },
	{ .name = "eva_56", .size = 512, MC_MICRO,
	  .cksum = 0x000, .chan = 0x0E0, .band = 0x0DC, .refdiv = 0x0D4,
	  .nchan = 32, .stride = 8, .tx = 2, .rx = 5, .numbered = 1, NF(FLAGS_EVA),
	  .pl_tone = 0x047, .pl_list = 0x047, .pl_count = 0x0CE, .pl_mode = 0x1FD,
	  .trak = 1, .trak_ct = 0x0DA, .size_flag = 0x0CF,
	  .pl_max = 10, .pl_k = MC_PL_K_EVA, NT(TIMERS_EVA),
	  .about = "EVA, 5/6-tone signalling -- MCEV_56" },
	{ .name = "eza_sel5", .size = 256, MC_MICRO,
	  .cksum = 0x000, .chan = 0x0C8, .band = 0x082, .refdiv = 0x0C4,
	  .nchan = 8, .stride = 6, .tx = 0, .rx = 3, NF(FLAGS_EZA9),
	  .pl_tone = 0x02F, .pl_list = 0x031, .pl_count = 0x083, .pl_mode = 0x07F,
	  .pl_max = 10, .pl_k = MC_PL_K_EVA, .aak = 0x076, .wcount = 0x09E, .wcount_clr = 0x80,
	  .about = "EZA, SEL5 signalling -- MCEZ9 and its R/M builds" },
	/* MCEZ13: no mode byte, an encoder table and a decoder table, and its checksum covers the
	 * WHOLE 128 bytes -- the sum loop at CS:0x767E runs 0..size-1 with size = 128, and the editor
	 * zeroes 0x003 and stores the complement there (watched at CS:0x7B15/0x7B34).
	 *
	 * These offsets were all two bytes low until the ident was fixed.  tools/eza.py used to strip
	 * two leading bytes off the INITIALIZE capture to make it read back, and every offset here was
	 * then derived from the shifted image.  The strip was compensating for a malformed synthetic
	 * ident, not for anything the radio does -- see ../doc/EEPROM_MAP_EZA.md. */
	{ .name = "eza_cspl", .size = 128, MC_MICRO,
	  .cksum = 0x003, .chan = 0x03B, .band = 0x039, .refdiv = 0x004,
	  .nchan = 8, .stride = 6, .tx = 0, .rx = 3, NF(FLAGS_EZ13),
	  .pl_tone = 0x024, .pl_list = 0x024, .pl_dec = 0x010,
	  .pl_max = 10, .pl_k = MC_PL_K_EZ13,
	  .about = "EZA, CS/PL -- MCEZ13, the 128-byte variant" },

	/* ---- Radius M110 -------------------------------------------------------------------------
	 * A different radio that answers the same wire protocol.  Nothing else is shared: the checksum
	 * sums to 0x01 rather than 0xFF, the byte lives at 0x0F rather than 0x000, the band is four
	 * bits at the bottom of 0x0A rather than three in the middle of another byte, and no MC micro
	 * offset lands on a real field -- `eza_sel5's write counter at 0x09E is live channel data on
	 * both M110 families.
	 *
	 * The family tag at 0x07..0x09 is a documented field: the RSS's radio-type descriptor table
	 * (M110/MRAR0200.EXE, file 0x31798) holds {&"EZ3.01.00.44", &"EZA", 0x000F, 0} and
	 * {&"EZ9.01.00.45", &"EZ9", 0x000F, 0} -- which is also where the checksum offset 0x0F is
	 * confirmed statically for both.
	 *
	 * Measured on four radios (reports/run3, run4, run5, write-runs/report8) and cross-checked by
	 * serving each image back to the 1989 RSS under emulation, which decoded every one.
	 *
	 * NOT established, hence zero here: the PL encoding (the CSQ/PL record carries PLE at +0 and
	 * PLD at +5, but the tone scale is unmeasured), the flag bits, and the timer block. */
	{ .name = "m110_cspl", .size = 256,
	  .cksum = 0x00F, .cksum_len = 128, .cksum_target = 0x01, .write_len = 128,
	  .chan = 0x01B, .band = 0x00A, .band_shift = 0, .band_mask = 0x0F,
	  .band_p = BAND_P_M110, .band_n = (uint8_t)(sizeof BAND_P_M110), .tag = "EZA", .tag_off = 7,
	  .refdiv = 0x013, .nchan = M110_CSPL_NCHAN, .stride = 10, .tx = 2, .rx = 7,
	  /* K-14a: PL per channel, in the record -- encode at +0, decode at +5, straddling the TX
	   * triplet.  Both laws are the ones MCEZ13 already uses, and the M110 RSS carries them as
	   * IEEE doubles: 7.9844 for the encoder and 61.107 for the decoder (MRAR0200.EXE 0x3174D).
	   * Confirmed on hardware: 123.0 Hz stores 982 (= 123.0 x 7.9844) and 7516 (= x 61.107). */
	  .pl_ch_enc = 0, .pl_ch_dec = 5, .pl_k = MC_PL_K_EZ13,
	  .about = "Radius M110, CSQ/PL -- EZ3.01.00.44; 128 real bytes mirrored into 256" },
	{ .name = "m110_sel5", .size = 256,
	  .cksum = 0x00F, .cksum_target = 0x01,
	  .chan = 0x092, .band = 0x00A, .band_shift = 0, .band_mask = 0x0F,
	  .band_p = BAND_P_M110, .band_n = (uint8_t)(sizeof BAND_P_M110), .tag = "EZ9", .tag_off = 7,
	  .refdiv = 0x089, .nchan = M110_SEL5_NCHAN, .stride = 12, .tx = 1, .rx = 4,
	  /* Sel 5 signalling, so no PL at all -- and 0 is a real offset, hence explicit. */
	  .pl_ch_enc = MC_PL_NONE, .pl_ch_dec = MC_PL_NONE,
	  .about = "Radius M110, Sel 5 -- EZ9.01.00.45" },
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

/* The signalling marker a real EVA puts in its ident.  Observed once, in the 2009 capture:
 * "EV9.01.00.11 455M11-3     5/6 Tone radio".  There is no SEL5 counterpart on record, so this
 * can only ever CONFIRM 5/6-tone -- its absence proves nothing and falls back to the table order. */
static const char TONE56[] = "5/6 Tone";

static int ident_has(const char *ident, size_t ilen, const char *needle)
{
	size_t n = strlen(needle), i;
	if (!ident || ilen < n)
		return 0;
	for (i = 0; i + n <= ilen; i++)
		if (memcmp(ident + i, needle, n) == 0)
			return 1;
	return 0;
}

const mc_model *mc_model_detect_ident(const uint8_t *bytes, size_t len, const char *ident,
                                      size_t ilen, char *note, size_t notesz)
{
	const mc_model *m = mc_model_detect(bytes, len, note, notesz);

	/* Only ever narrows an ambiguity the bytes could not settle. */
	if (m && strstr(note, "models fit") && ident_has(ident, ilen, TONE56)) {
		const mc_model *t = mc_model_by_name("eva_56");
		if (t && t->size == len) {
			snprintf(note, notesz, "the radio's ident says \"%s\"; using %s", TONE56, t->name);
			return t;
		}
	}
	return m;
}

/* Does this image carry `m's identifying marker?  Models without one match vacuously, which keeps
 * every MC micro model behaving exactly as before. */
static int tag_matches(const mc_model *m, const uint8_t *bytes, size_t len)
{
	size_t n;

	if (!m->tag)
		return 1;
	n = strlen(m->tag);
	if ((size_t)m->tag_off + n > len)
		return 0;
	return memcmp(bytes + m->tag_off, m->tag, n) == 0;
}

/* Does it carry some OTHER model's marker?  Used to stop an unmarked model claiming bytes that
 * announce themselves as something else -- the M110-read-as-eza_sel5 case. */
static int foreign_tag(const uint8_t *bytes, size_t len, const mc_model *self)
{
	const mc_model *m;
	size_t i;

	for (i = 0; (m = mc_model_by_index(i)) != NULL; i++)
		if (m->tag && m != self && tag_matches(m, bytes, len))
			return 1;
	return 0;
}

const mc_model *mc_model_marked(const uint8_t *bytes, size_t len)
{
	const mc_model *m;
	size_t i;

	for (i = 0; (m = mc_model_by_index(i)) != NULL; i++)
		if (m->tag && m->size == len && tag_matches(m, bytes, len))
			return m;
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
		if (m->size != len || !tag_matches(m, bytes, len) || !mc_checksum_valid(&t))
			continue;
		/* A model with no marker of its own must not claim bytes that carry someone else's.  An
		 * M110 CSQ/PL image sums to 0x02 over 256 and would fail `eza_sel5's checksum anyway, but
		 * relying on that is relying on a coincidence of two unrelated rules. */
		if (!m->tag && foreign_tag(bytes, len, m))
			continue;
		if (!best)
			best = m;
		n++;
		snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", n > 1 ? ", " : "",
		         m->name);
	}
	if (!best) {
		/* Three different failures, and telling them apart is the whole value of this message.
		 * The old text named the first model of the right size whatever the bytes said, so an
		 * M110 was reported as "matches model eza_sel5, but its checksum is invalid" -- which
		 * invited `--model eza_sel5', the one action that turns a refusal into a corruption. */
		const mc_model *sized = NULL, *tagged = NULL;
		for (i = 0; (m = mc_model_by_index(i)) != NULL; i++) {
			if (m->size != len)
				continue;
			if (m->tag && tag_matches(m, bytes, len) && !tagged)
				tagged = m;
			if (!sized && !foreign_tag(bytes, len, m))
				sized = m;
		}
		if (tagged) {
			/* The markers say what it is; only the checksum disagrees.  Now naming the model is
			 * useful rather than misleading. */
			mc_image t;
			t.model = tagged;
			t.bytes = (uint8_t *)bytes;
			t.len = len;
			snprintf(note, notesz,
			         "carries the %s marker of model %s, but its checksum is invalid (the covered "
			         "bytes sum to 0x%02X, should be 0x%02X) -- the codeplug is damaged",
			         tagged->tag, tagged->name, mc_checksum_total(&t), tagged->cksum_target);
		} else if (sized) {
			mc_image t;
			t.model = sized;
			t.bytes = (uint8_t *)bytes;
			t.len = len;
			snprintf(note, notesz,
			         "%u bytes matches model %s, but its checksum is invalid (sums to 0x%02X, "
			         "should be 0x%02X)", (unsigned)len, sized->name, mc_checksum_total(&t),
			         sized->cksum_target);
		} else {
			/* Right size, but the markers belong to a model whose other tests failed.  Do NOT
			 * name a model here: every name offered is an invitation to --model, which skips
			 * detection entirely and lets a wrong layout write to a radio. */
			int any = 0;
			for (i = 0; (m = mc_model_by_index(i)) != NULL; i++)
				if (m->size == len)
					any = 1;
			if (any)
				snprintf(note, notesz,
				         "%u bytes, but no model's markers match -- this is not a codeplug any "
				         "model in this build describes", (unsigned)len);
			else
				snprintf(note, notesz, "no model is %u bytes", (unsigned)len);
		}
	}
	else if (n > 1)
		snprintf(note, notesz, "%u models fit (%s); assuming %s -- use --model to choose",
		         (unsigned)n, list, best->name);
	else
		snprintf(note, notesz, "detected %s", best->name);
	return best;
}
