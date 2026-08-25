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
/* Conformance tests against testdata/, citing spec.md requirement numbers.
 *
 *     ./test_vectors [repo-root]        default repo root is .
 *
 * Every check is driven by a committed data file rather than by constants written here, so the Go
 * implementation runs the identical suite and any disagreement localises to one vector.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/codeplug.h"

static const char *ROOT = ".";
static int pass, fail;

static void ok(int cond, const char *req, const char *what)
{
	if (cond) {
		pass++;
	} else {
		fail++;
		printf("FAIL  [%s] %s\n", req, what);
	}
}

static void failf(const char *req, const char *fmt, ...)
{
	va_list ap;
	fail++;
	printf("FAIL  [%s] ", req);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static void test_detect_ident(void);

static char *slurp(const char *rel, size_t *len)
{
	char path[512];
	FILE *f;
	char *buf;
	long n;

	snprintf(path, sizeof path, "%s/%s", ROOT, rel);
	f = fopen(path, "rb");
	if (!f) {
		printf("FAIL  cannot open %s\n", path);
		fail++;
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	buf[n] = 0;
	fclose(f);
	if (len)
		*len = (size_t)n;
	return buf;
}

/* K-20: size and checksum cannot separate the two 512-byte models; the radio's ident can.
 *
 * Both are EVA 9 hardware differing only in signalling, so a bare file is genuinely ambiguous and
 * the table order is a preference.  The one real EVA ident on record says "5/6 Tone radio", and
 * that is the only marker there is -- no SEL5 ident has ever been captured, so its ABSENCE must
 * not be treated as evidence.  These check both halves. */
static void test_detect_ident(void)
{
	uint8_t *img;
	size_t len;
	char note[256];
	const mc_model *m;
	static const char REAL[] = "EV9.01.00.11 455M11-3     5/6 Tone radio";
	static const char SILENT[] = "EV9.01.00.11 455M11-3     no idea";

	img = (uint8_t *)slurp("fixtures/eva9_real.bin", &len);
	if (!img)
		return;
	m = mc_model_detect(img, len, note, sizeof note);
	ok(m && strcmp(m->name, "eva_sel5") == 0, "K-20",
	   "with no ident the 512-byte preference is eva_sel5");
	ok(strstr(note, "models fit") != NULL, "K-20", "and the ambiguity is reported, not hidden");

	m = mc_model_detect_ident(img, len, REAL, sizeof REAL - 1, note, sizeof note);
	ok(m && strcmp(m->name, "eva_56") == 0, "K-20",
	   "an ident saying 5/6 Tone settles it on eva_56");
	ok(strstr(note, "ident") != NULL && strstr(note, "models fit") == NULL, "K-20",
	   "and the note says the ident decided, not that it guessed");

	m = mc_model_detect_ident(img, len, SILENT, sizeof SILENT - 1, note, sizeof note);
	ok(m && strcmp(m->name, "eva_sel5") == 0, "K-20",
	   "an ident without the marker falls back to the preference, concluding nothing");
	ok(strstr(note, "models fit") != NULL, "K-20", "and says so");

	m = mc_model_detect_ident(img, len, NULL, 0, note, sizeof note);
	ok(m && strcmp(m->name, "eva_sel5") == 0, "K-20", "a NULL ident behaves as mc_model_detect");
	free(img);
}


/* ---- codeplug golden decodes (K-2, K-10, K-20, K-21, K-23, K-24) ---------------------------- */

static void test_codeplug(const char *vecrel)
{
	char *vec = slurp(vecrel, NULL), *img_bytes;
	char imgpath[256] = "", modelname[64] = "";
	size_t imglen = 0;
	const mc_model *model;
	mc_image img;
	FILE *tmp;
	char *line, *save, *want, *got = NULL;
	size_t gotlen = 0;
	int lineno = 0, mismatch = 0;

	if (!vec)
		return;
	/* the expected file names its own inputs */
	for (line = strtok_r(vec, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (sscanf(line, "IMG %255s", imgpath) == 1)
			break;
	}
	for (line = strtok_r(NULL, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (sscanf(line, "MODEL %63s", modelname) == 1)
			break;
	}
	free(vec);
	if (!imgpath[0] || !modelname[0]) {
		failf("K-20", "%s: no IMG/MODEL header", vecrel);
		return;
	}
	model = mc_model_by_name(modelname);
	if (!model) {
		failf("K-20", "%s: unknown model %s", vecrel, modelname);
		return;
	}
	img_bytes = slurp(imgpath, &imglen);
	if (!img_bytes)
		return;
	img.model = model;
	img.bytes = (uint8_t *)img_bytes;
	img.len = imglen;

	/* re-render and compare with the expected file, line by line */
	tmp = tmpfile();
	mc_dump_vec(tmp, &img, imgpath);
	fseek(tmp, 0, SEEK_END);
	gotlen = (size_t)ftell(tmp);
	fseek(tmp, 0, SEEK_SET);
	got = malloc(gotlen + 1);
	if (fread(got, 1, gotlen, tmp) != gotlen) {
		failf("K-20", "%s: short read of rendered output", vecrel);
		fclose(tmp);
		free(got);
		free(img_bytes);
		return;
	}
	got[gotlen] = 0;
	fclose(tmp);

	want = slurp(vecrel, NULL);
	{
		char *wl, *ws, *gl, *gs;
		gl = strtok_r(got, "\n", &gs);
		for (wl = strtok_r(want, "\n", &ws); wl; wl = strtok_r(NULL, "\n", &ws)) {
			if (wl[0] == '#')
				continue; /* generator provenance, not part of the contract */
			lineno++;
			if (!gl) {
				failf("K-20", "%s line %d: expected %s, output ended", vecrel, lineno, wl);
				mismatch = 1;
				break;
			}
			if (strcmp(wl, gl) != 0) {
				failf("K-20", "%s line %d:\n        want %s\n        got  %s", vecrel,
				      lineno, wl, gl);
				mismatch = 1;
				break;
			}
			gl = strtok_r(NULL, "\n", &gs);
		}
		if (!mismatch && gl)
			failf("K-20", "%s: %d expected lines but output continues with %s", vecrel,
			      lineno, gl);
		else if (!mismatch)
			pass++;
	}
	free(want);
	free(got);

	/* The image must contain everything the model addresses.  Without this every accessor reads
	 * past the end of a truncated file -- confirmed with AddressSanitizer before it was added. */
	ok(mc_image_check(&img) == 0, "K-20", "the fixture satisfies its model's bounds");
	{
		mc_image probe = img;
		size_t need, i;
		/* asked of an empty image, the check reports how many bytes the model addresses */
		probe.len = 0;
		need = mc_image_check(&probe);
		ok(need > 0 && need <= img.len, "K-20", "the model reports a sane addressed extent");
		probe.len = need - 1;
		ok(mc_image_check(&probe) != 0, "K-20", "one byte below that extent is rejected");
		probe.len = need;
		ok(mc_image_check(&probe) == 0, "K-20", "exactly that extent is accepted");
		{
			mc_image shortimg = probe;
			(void)shortimg;
		}
		/* and every other model that is larger than this file must be rejected too */
		for (i = 0; mc_model_by_index(i); i++) {
			const mc_model *other = mc_model_by_index(i);
			mc_image t = img;
			t.model = other;
			if (other->size > img.len)
				ok(mc_image_check(&t) != 0, "K-20",
				   "a model larger than the file is rejected");
		}
	}

	/* K-2: the covered range is per-model.  MCEZ13 excludes its last two bytes, and no fixture
	 * exercises that because theirs happen to be zero -- so set one and check it is ignored. */
	if (model->cksum_len && model->cksum_len < img.len) {
		uint8_t keep = img.bytes[img.len - 1];
		uint8_t before = mc_checksum_total(&img);
		img.bytes[img.len - 1] = (uint8_t)(keep + 0x5A);
		ok(mc_checksum_total(&img) == before, "K-2",
		   "bytes past cksum_len are outside the checksum");
		img.bytes[img.len - 1] = keep;
	}

	/* K-2: fixing the checksum of a deliberately corrupted copy restores the original byte */
	{
		uint8_t saved = img.bytes[model->cksum];
		int was_valid = mc_checksum_valid(&img);
		img.bytes[model->cksum] ^= 0x5A;
		mc_checksum_fix(&img);
		ok(mc_checksum_valid(&img), "K-2", "checksum_fix produces a valid image");
		if (was_valid)
			ok(img.bytes[model->cksum] == saved, "K-2",
			   "checksum_fix reproduces the original stored byte");
		img.bytes[model->cksum] = saved;
	}
	free(img_bytes);
}

/* ---- frequency codec (K-10, K-11) ----------------------------------------------------------- */

static void test_freq(void)
{
	char *vec = slurp("testdata/freq/roundtrip.vec", NULL), *line, *save;
	if (!vec)
		return;
	for (line = strtok_r(vec, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char rawhex[16], wanthex[16];
		unsigned p, hz, step, flags, canon;
		uint8_t raw[3], out[3];

		if (sscanf(line, "DEC raw=%15s p=%u hz=%u canon=%u", rawhex, &p, &hz, &canon) == 4) {
			unsigned b0, b1, b2;
			sscanf(rawhex, "%2x%2x%2x", &b0, &b1, &b2);
			raw[0] = (uint8_t)b0;
			raw[1] = (uint8_t)b1;
			raw[2] = (uint8_t)b2;
			ok(mc_freq_decode(raw, p) == hz, "K-10", "decode");
			ok((unsigned)mc_freq_is_canonical(raw, p) == canon, "K-11", "canonical flag");
			/* Round-trip.  Canonical fields must come back byte-identical; non-canonical
			 * ones must NOT (that is the point of K-11) but must decode to the same Hz. */
			step = (raw[0] & 4) ? 3125 : 2500;
			if (mc_freq_encode(hz, p, step, raw[0], out) != 0) {
				failf("K-11", "re-encode of %s (%u Hz) failed", rawhex, hz);
			} else if (canon) {
				ok(memcmp(out, raw, 3) == 0, "K-11", "canonical field round-trips exactly");
			} else {
				ok(memcmp(out, raw, 3) != 0, "K-11",
				   "non-canonical field re-encodes to the canonical spelling");
				ok(mc_freq_decode(out, p) == hz, "K-11",
				   "and the canonical spelling decodes to the same frequency");
			}
		} else if (sscanf(line, "ENC hz=%u p=%u step=%u flags=%x want=%15s", &hz, &p, &step,
		                  &flags, wanthex) == 5) {
			char gothex[8];
			if (mc_freq_encode(hz, p, step, (uint8_t)flags, out) != 0) {
				failf("K-11", "encode of %u Hz failed", hz);
				continue;
			}
			snprintf(gothex, sizeof gothex, "%02x%02x%02x", out[0], out[1], out[2]);
			if (strcmp(gothex, wanthex) != 0)
				failf("K-11", "encode %u Hz p=%u: want %s got %s", hz, p, wanthex, gothex);
			else
				pass++;
			ok(out[2] < p, "K-11", "encoder emits b2 < P");
		}
	}
	free(vec);
}

/* ---- software parity (P-2) ------------------------------------------------------------------ */

static void test_parity(void)
{
	char *vec = slurp("testdata/parity/parity.vec", NULL), *line, *save;
	if (!vec)
		return;
	for (line = strtok_r(vec, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		unsigned in, out, val, okflag;
		uint8_t got;
		if (sscanf(line, "TX in=%x out=%x", &in, &out) == 2) {
			ok(mc_parity_tx((uint8_t)in) == out, "P-2", "transmit parity");
		} else if (sscanf(line, "RX in=%x ok=%u val=%x", &in, &okflag, &val) == 3) {
			int r = mc_parity_rx((uint8_t)in, &got);
			ok((r == 0) == (okflag == 1), "P-2", "receive parity verdict");
			ok(got == val, "P-2", "receive value is masked to 7 bits");
		}
	}
	free(vec);
}

/* ---- PL / CTCSS (K-14) ----------------------------------------------------------------------- */

static void test_pl(void)
{
	static const char *FILES[] = { "testdata/pl/pl.vec", "testdata/timers/timers.vec",
	                               "testdata/wcount/wcount.vec" };
	size_t vi;

	for (vi = 0; vi < sizeof FILES / sizeof FILES[0]; vi++) {
	char *vec = slurp(FILES[vi], NULL), *line, *save;
	if (!vec)
		continue;
	for (line = strtok_r(vec, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char mname[64], tname[64];
		unsigned idx, dhz, word, tone, list, cnt, mode, dec, mx, off, mask, wid, den, add;

		if (sscanf(line, "TONE idx=%u dhz=%u word=%x", &idx, &dhz, &word) == 3) {
			ok(mc_pl_standard(idx) == dhz, "K-14", "the standard tone list matches");
			ok(mc_pl_encode(dhz) == word, "K-14", "and each entry encodes as expected");
		} else if (sscanf(line, "PLENC dhz=%u word=%x", &dhz, &word) == 2) {
			ok(mc_pl_encode(dhz) == word, "K-14", "encode round(7.984 x f)");
		} else if (sscanf(line, "PLDEC word=%x dhz=%u", &word, &dhz) == 2) {
			ok(mc_pl_decode((uint16_t)word) == dhz, "K-14",
			   "decode, snapped to the standard list where one matches");
		} else if (sscanf(line, "PLENCK dhz=%u k=%u word=%x", &dhz, &cnt, &word) == 3) {
			ok(mc_pl_encode_k(dhz, cnt) == word, "K-14",
			   "the per-build tone scale: EVA 7.9840, MCEZ13 7.9844");
			ok(mc_pl_decode_k((uint16_t)word, cnt) == dhz, "K-14",
			   "and it round-trips under its own scale");
		} else if (sscanf(line, "PLDENC dhz=%u word=%x", &dhz, &word) == 2) {
			ok(mc_pl_dec_encode(dhz) == word, "K-14",
			   "the MCEZ13 decoder law, round(61.107 x f)");
		} else if (sscanf(line, "PLDDEC word=%x dhz=%u", &word, &dhz) == 2) {
			ok(mc_pl_dec_decode((uint16_t)word) == dhz, "K-14",
			   "and decodes back, snapped to the standard list");
		} else if (sscanf(line, "PLMAP model=%63s tone=%x list=%x count=%x mode=%x dec=%x max=%u",
		                  mname, &tone, &list, &cnt, &mode, &dec, &mx) == 7) {
			const mc_model *m = mc_model_by_name(mname);
			if (!m) {
				failf("K-14", "unknown model %s", mname);
				continue;
			}
			ok(m->pl_tone == tone && m->pl_list == list && m->pl_count == cnt &&
			       m->pl_mode == mode && m->pl_dec == dec && m->pl_max == mx,
			   "K-14", "the per-model PL layout matches");
		} else if (sscanf(line, "TMRDEC %x %u %u %u %x %u", &mask, &wid, &den, &add, &word,
		                  &dhz) == 6) {
			mc_timer t;
			t.name = "vec"; t.off = 0;
			t.mask = (uint16_t)mask; t.width = (uint8_t)wid;
			t.den = (uint16_t)den; t.add_ms = (uint16_t)add;
			ok(mc_timer_decode(&t, word) == dhz, "K-16",
			   "the timer decode law reproduces its measured point");
		} else if (sscanf(line, "TIMER %63s %u %63s %x %x %u %u %u", mname, &idx, tname, &off,
		                  &mask, &wid, &den, &add) == 8) {
			const mc_model *m = mc_model_by_name(mname);
			const mc_timer *t = m ? mc_timer_at(m, idx) : NULL;
			char want[64];
			size_t c;
			for (c = 0; tname[c] && c + 1 < sizeof want; c++)
				want[c] = tname[c] == '_' ? ' ' : tname[c];
			want[c] = '\0';
			ok(t && t->off == off && t->mask == mask && t->width == wid && t->den == den &&
			       t->add_ms == add && strcmp(t->name, want) == 0,
			   "K-16", "the model's timer table matches the original's own");
		} else if (sscanf(line, "TMRRT %u %u", &idx, &dhz) == 2) {
			const mc_model *m = mc_model_by_name("eva_56");
			const mc_timer *t = mc_timer_at(m, idx);
			uint8_t buf[512];
			mc_image scratch;
			scratch.model = m; scratch.bytes = buf; scratch.len = sizeof buf;
			memset(buf, 0xFF, sizeof buf);   /* every flag bit outside the field set */
			if (!t) {
				failf("K-16", "no timer %u", idx);
			} else {
				unsigned keep = t->width == 1 ? 0xFFu & ~(unsigned)t->mask
				                              : 0xFFFFu & ~(unsigned)t->mask;
				unsigned raw;
				int rc = mc_timer_set_ms(&scratch, idx, dhz);
				raw = t->width == 1 ? buf[t->off]
				                    : (unsigned)(buf[t->off] << 8) | buf[t->off + 1];
				ok(rc == 0 && mc_timer_get_ms(&scratch, idx) == dhz && (raw & ~(unsigned)t->mask)
				       == keep,
				   "K-16", "the timer round-trips and leaves the flags beside it alone");
			}
		} else if (sscanf(line, "TMRNO %u %u", &idx, &dhz) == 2) {
			const mc_model *m = mc_model_by_name("eva_56");
			uint8_t buf[512];
			mc_image scratch;
			scratch.model = m; scratch.bytes = buf; scratch.len = sizeof buf;
			memset(buf, 0x5A, sizeof buf);
			ok(mc_timer_set_ms(&scratch, idx, dhz) != 0 && buf[0xB3] == 0x5A, "K-16",
			   "U-3: a value the timer law cannot spell is refused and writes nothing");
		} else if (sscanf(line, "WCOUNT %63s %x %x", mname, &off, &mask) == 3) {
			const mc_model *m = mc_model_by_name(mname);
			ok(m && m->wcount == off && m->wcount_clr == mask, "W-5",
			   "the model carries the write counter that was measured for it");
		} else if (sscanf(line, "WCNEXT %63s %x %x", mname, &word, &dhz) == 3) {
			const mc_model *m = mc_model_by_name(mname);
			ok(m && mc_write_counter_next(m, (uint8_t)word) == (uint8_t)dhz, "W-5",
			   "one write advances the counter exactly as the radio saw it");
		} else if (sscanf(line, "PLMODELK model=%63s k=%u", mname, &cnt) == 2) {
			const mc_model *m = mc_model_by_name(mname);
			if (!m) {
				failf("K-14", "unknown model %s", mname);
				continue;
			}
			ok(m->pl_k == cnt, "K-14",
			   "the model carries the tone scale its own chain file holds");
		}
	}
	free(vec);

	/* Round-trip through a real image: set a tone, read it back, and check nothing else moved. */
	{
		size_t len = 0;
		uint8_t *img_bytes = (uint8_t *)slurp("fixtures/eva9_real.bin", &len);
		if (img_bytes) {
			mc_image im;
			uint8_t before[512];
			size_t i, moved = 0;
			im.model = mc_model_by_name("eva_sel5");
			im.bytes = img_bytes;
			im.len = len;
			memcpy(before, img_bytes, len);
			mc_pl_set_mode(&im, MC_PL_SINGLE);
			ok(mc_pl_get_mode(&im) == MC_PL_SINGLE, "K-14", "mode reads back as set");
			ok(mc_pl_set_tone(&im, 0, 885) == 0, "K-14", "88.5 Hz is accepted");
			ok(mc_pl_get_tone(&im, 0) == 885, "K-14",
			   "and reads back as 88.5, not 88.55 -- the snap works");
			ok(img_bytes[0x047] == 0x02 && img_bytes[0x048] == 0xC3, "K-14",
			   "stored as 02C3, exactly what the 1987 editor wrote");
			ok((img_bytes[0x1FD] & 0xF0) == 0x60, "K-14", "and the mode byte is 0x60");
			for (i = 0; i < len; i++)
				if (before[i] != img_bytes[i])
					moved++;
			ok(moved == 3, "K-30", "setting PL moved exactly the tone word and the mode byte");
			ok(mc_pl_set_tone(&im, 0, 100) != 0, "U-3",
			   "a frequency below the radio's range is refused, not clamped");
			/* The count lives in the HIGH nibble; the low one is the selectable-lockout
			 * marker and must survive (K-30).  No fixture distinguishes the two nibbles,
			 * so it is exercised directly. */
			mc_pl_set_mode(&im, MC_PL_SELECTABLE);
			img_bytes[im.model->pl_count] = 0xA5;
			mc_pl_set_count(&im, 3);
			ok(mc_pl_get_count(&im) == 3, "K-14", "the tone count reads back as set");
			ok((img_bytes[im.model->pl_count] >> 4) == 3, "K-14",
			   "and it is stored in the high nibble");
			ok((img_bytes[im.model->pl_count] & 0x0F) == 5, "K-30",
			   "the low nibble, the selectable-lockout marker, is preserved");
			/* K-10a.  The reference dividers decode as word = 2*(Fref/spacing)+1, from
			 * REF_DIV.001 on the original disks.  Reporting only -- K-30 keeps them verbatim --
			 * so this is deliberately NOT wired into the dump, whose golden vectors are an
			 * independent expectation and would stop testing anything if both sides moved. */
			ok(mc_refdiv_spacing(0x1681) == 5000, "K-10a", "0x1681 is the 14.4 MHz 5 kHz divider");
			ok(mc_refdiv_spacing(0x1201) == 6250, "K-10a", "0x1201 is 14.4 MHz at 6.25 kHz");
			ok(mc_refdiv_spacing(0x1401) == 5000, "K-10a", "0x1401 is the SP 12.8 MHz 5 kHz divider");
			ok(mc_refdiv_spacing(0x1001) == 6250, "K-10a", "0x1001 is SP at 6.25 kHz");
			ok(mc_refdiv_spacing(0x1683) == 0, "K-10a",
			   "a word that is merely odd is not thereby a divider");
			ok(mc_refdiv_spacing(0x0000) == 0, "K-10a", "and zero is not one either");
			/* every EVA sample carries the standard pair, in that order */
			ok(mc_refdiv(&im, 0) == 0x1681 && mc_refdiv(&im, 1) == 0x1201, "K-10a",
			   "the sample carries the standard 5 kHz / 6.25 kHz pair");

			mc_pl_set_count(&im, 99);
			ok(mc_pl_get_count(&im) == im.model->pl_max, "K-14",
			   "an out-of-range count is clamped to the model's maximum");

			/* K-30a.  The mode byte's LOW nibble is the radio's index into the tone list --
			 * EZA33 F270 reads cp_pl_list[low nibble] at 0x047 + 2*i.  In single-tone mode the
			 * tone is written to slot 0, so a stale nibble makes the radio read a different
			 * slot than the one we wrote.  The 1987 RSS assigns 0x60 outright (the sweep caught
			 * 0xE7 -> 0x60); we must too.  Masking with 0xF0 hides this, so check the byte
			 * whole. */
			img_bytes[im.model->pl_mode] = 0xE7; /* selectable, operator on tone 7 */
			mc_pl_set_mode(&im, MC_PL_SINGLE);
			ok(img_bytes[im.model->pl_mode] == 0x60, "K-30a",
			   "single-tone mode clears the list index, so the radio reads the slot we wrote");
			/* Selectable keeps the operator's choice, but never past the populated entries. */
			mc_pl_set_count(&im, 4);
			img_bytes[im.model->pl_mode] = (uint8_t)(0xE0 | 2);
			mc_pl_set_mode(&im, MC_PL_SELECTABLE);
			ok(img_bytes[im.model->pl_mode] == 0xE2, "K-30a",
			   "selectable preserves an in-range list index");
			img_bytes[im.model->pl_mode] = (uint8_t)(0xE0 | 9);
			mc_pl_set_mode(&im, MC_PL_SELECTABLE);
			ok(img_bytes[im.model->pl_mode] == (uint8_t)(0xE0 | 3), "K-30a",
			   "and clamps one that points past the last populated tone");
			ok((img_bytes[im.model->pl_mode] & 0x40) != 0, "K-30a",
			   "bit 6, which gates PL encode in the radio, is set in both modes");

			/* K-31.  The radio does not take the codeplug size on trust: it reads bit 7 of the
			 * size-flag byte and computes the trakmode block down from the top (EZA33 §7f).
			 * MCprog used to hardcode 0x1FD, which is only right for a 512-byte codeplug. */
			ok(mc_codeplug_top(&im) == 512, "K-31",
			   "a codeplug with the size flag clear is 512 bytes");
			ok(mc_trak_base(&im, 0) == 0x1E5, "K-31",
			   "TrakBase(0) = 512 - 27 = 0x1E5, matching the RSS routine at 0x3A78");
			ok(mc_pl_mode_off(&im) == 0x1FD, "K-31",
			   "so the PL mode byte derives to 0x1FD -- the value that used to be hardcoded");
			ok(mc_pl_mode_off(&im) == im.model->pl_mode, "K-31",
			   "and the static field agrees, so 512-byte behaviour is unchanged");
			{
				/* Flip the size flag and the whole block must move with it.  The image is only
				 * 512 bytes here, so mc_trak_base() must refuse rather than point outside it --
				 * that refusal is the safety property worth pinning. */
				uint8_t save = img_bytes[im.model->size_flag];
				img_bytes[im.model->size_flag] = (uint8_t)(save | 0x80);
				ok(mc_codeplug_top(&im) == 1024, "K-31",
				   "setting bit 7 of the size flag makes it a 1024-byte codeplug");
				ok(mc_trak_base(&im, 0) == 0, "K-31",
				   "and a block that would fall outside a short image is refused, not returned");
				ok(mc_pl_mode_off(&im) == im.model->pl_mode, "K-31",
				   "so the PL offset falls back to the static field rather than running off the end");
				img_bytes[im.model->size_flag] = save;
			}
			ok(mc_trak_base(&im, 99) == 0, "K-31",
			   "an absurd trakmode index is refused");
			ok(mc_channel_trak(&im, 0) == img_bytes[im.model->chan + 1], "K-31",
			   "a channel's trakmode is record byte +1");
			ok(mc_channel_trak(&im, im.model->nchan) == -1, "K-31",
			   "and an out-of-range channel yields -1, not a stray byte");
			ok(mc_pl_supported(mc_model_by_name("eza_cspl")) == 1, "K-14",
			   "MCEZ13 does have PL, now that its read is solved");
			ok(mc_pl_has_decoder(mc_model_by_name("eza_cspl")) == 1, "K-14",
			   "and it is the only model that decodes PL as well as encoding it");
			ok(mc_pl_has_decoder(mc_model_by_name("eva_sel5")) == 0, "K-14",
			   "the EVA and EZA 9 encode only -- there is no PL decode on them");
			free(img_bytes);
		}
	}
	}
}

/* ---- edits (K-11, K-22, K-30, U-3) ---------------------------------------------------------- */

/* K-15, the auto-acknowledge delay.  Only the repair build of the 1987 software exposes it, so
 * every number here traces back to driving that build; see ../doc/BUILD_VARIANTS.md. */
static void test_aak(void)
{
	char *vec = slurp("testdata/aak/aak.vec", NULL), *line, *save;
	if (!vec)
		return;
	for (line = strtok_r(vec, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char mname[64];
		unsigned ms, count, off;

		if (sscanf(line, "AAKENC ms=%u count=%u", &ms, &count) == 2) {
			ok(mc_aak_encode_ms(ms) == count, "K-15", "encode round(ms / 15.625)");
		} else if (sscanf(line, "AAKDEC count=%u ms=%u", &count, &ms) == 2) {
			ok(mc_aak_decode_count(count) == ms, "K-15", "decode round(count x 15.625)");
		} else if (sscanf(line, "AAKBAD ms=%u", &ms) == 1) {
			size_t len = 0;
			uint8_t *b = (uint8_t *)slurp("fixtures/eza9_default_band2.bin", &len);
			if (b) {
				mc_image im;
				uint8_t was;
				im.model = mc_model_by_name("eza_sel5");
				im.bytes = b;
				im.len = len;
				was = b[im.model->aak];
				ok(mc_aak_set_ms(&im, ms) != 0, "U-3", "an out-of-range delay is refused");
				ok(b[im.model->aak] == was, "U-3", "and nothing is written when it is");
				free(b);
			}
		} else if (sscanf(line, "AAKMAP model=%63s off=%x", mname, &off) == 2) {
			const mc_model *m = mc_model_by_name(mname);
			if (!m) {
				failf("K-15", "unknown model %s", mname);
				continue;
			}
			ok(m->aak == off, "K-15", "the per-model offset matches");
			ok(mc_aak_supported(m) == (off != 0), "K-15",
			   "and a model without a measured offset reports no support");
		}
	}
	free(vec);

	/* Through a real image: set it, read it back, and check that nothing else moved -- including
	 * bit 7, which the original never sets and whose meaning is unknown (K-30). */
	{
		size_t len = 0;
		uint8_t *b = (uint8_t *)slurp("fixtures/eza9_default_band2.bin", &len);
		if (b) {
			mc_image im;
			uint8_t before[256];
			size_t i, moved = 0;
			im.model = mc_model_by_name("eza_sel5");
			im.bytes = b;
			im.len = len;
			memcpy(before, b, len);

			ok(mc_aak_get_ms(&im) == 203, "K-15", "the factory default reads 203 ms");
			ok(mc_aak_set_ms(&im, 500) == 0, "K-15", "500 ms is accepted");
			ok(mc_aak_get_ms(&im) == 500, "K-15", "and reads back as 500 ms");
			ok(b[im.model->aak] == 32, "K-15", "stored as a count of 32");
			for (i = 0; i < len; i++)
				moved += b[i] != before[i];
			ok(moved == 1, "K-30", "and only that one byte moved");

			/* bit 7 is preserved, not overwritten */
			b[im.model->aak] |= 0x80;
			ok(mc_aak_get_ms(&im) == 500, "K-15", "bit 7 is not part of the value");
			ok(mc_aak_set_ms(&im, 1000) == 0, "K-15", "a further edit is accepted");
			ok((b[im.model->aak] & 0x80) != 0, "K-30",
			   "and leaves bit 7 exactly as it found it");
			ok((b[im.model->aak] & 0x7F) == 64, "K-15", "while storing the new count");

			ok(mc_aak_set_ms(&im, MC_AAK_MIN_MS) == 0, "K-15", "the bottom of the range works");
			ok(mc_aak_get_ms(&im) == MC_AAK_MIN_MS, "K-15", "and round-trips");
			ok(mc_aak_set_ms(&im, MC_AAK_MAX_MS) == 0, "K-15", "so does the top");
			ok(mc_aak_get_ms(&im) == MC_AAK_MAX_MS, "K-15", "and round-trips");

			/* a count of 0 means "nothing here", not 0 ms */
			b[im.model->aak] = 0;
			ok(mc_aak_get_ms(&im) == 0, "K-15", "a stored count of 0 reads as unset");
			free(b);
		}
	}
	/* A model without the field must refuse rather than write at some guessed offset. */
	{
		size_t len = 0;
		uint8_t *b = (uint8_t *)slurp("fixtures/eva9_real.bin", &len);
		if (b) {
			mc_image im;
			uint8_t before[512];
			size_t i, moved = 0;
			im.model = mc_model_by_name("eva_sel5");
			im.bytes = b;
			im.len = len;
			memcpy(before, b, len);
			ok(!mc_aak_supported(im.model), "K-15", "the EVA has no measured AAK offset");
			ok(mc_aak_get_ms(&im) == 0, "K-15", "so it reports nothing");
			ok(mc_aak_set_ms(&im, 500) != 0, "K-15", "and refuses to set it");
			for (i = 0; i < len; i++)
				moved += b[i] != before[i];
			ok(moved == 0, "K-30", "writing not one byte in the attempt");
			free(b);
		}
	}
}

static void test_edits(void)
{
	char *vec = slurp("testdata/edit/edits.vec", NULL), *line, *save;
	if (!vec)
		return;
	for (line = strtok_r(vec, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char imgp[256], mname[64], op[64], changed[1024];
		int slot;
		long long arg;
		const mc_model *model;
		mc_image img;
		uint8_t *base;
		size_t blen = 0;
		int rc = 0;

		if (sscanf(line, "EDIT img=%255s model=%63s op=%63s slot=%d arg=%lld changed=%1023s",
		           imgp, mname, op, &slot, &arg, changed) != 6)
			continue;
		model = mc_model_by_name(mname);
		base = (uint8_t *)slurp(imgp, &blen);
		if (!model || !base) {
			failf("K-20", "%s / %s unavailable", imgp, mname);
			free(base);
			continue;
		}
		img.model = model;
		img.len = blen;
		img.bytes = malloc(blen);
		memcpy(img.bytes, base, blen);

		if (strcmp(op, "set_tx") == 0)
			rc = mc_channel_set_freq(&img, slot - 1, MC_TX, (uint32_t)arg);
		else if (strcmp(op, "set_rx") == 0)
			rc = mc_channel_set_freq(&img, slot - 1, MC_RX, (uint32_t)arg);
		else if (strncmp(op, "flag:", 5) == 0) {
			const mc_flag *fl = mc_flag_by_name(model, op + 5);
			if (!fl) {
				failf("K-22", "%s: no flag %s", mname, op + 5);
				goto next;
			}
			mc_flag_set(&img, slot - 1, fl, (int)arg);
			ok(mc_flag_get(&img, slot - 1, fl) == (int)arg, "K-22",
			   "the flag reads back as it was set");
		}

		if (strcmp(changed, "unrepresentable") == 0) {
			/* U-3: not representable must be an error, never a silent wrap. */
			ok(rc != 0, "U-3", "an out-of-range frequency is refused, not clamped");
			goto next;
		}
		if (rc != 0) {
			failf("K-11", "%s %s slot %d: refused a representable value", imgp, op, slot);
			goto next;
		}
		mc_checksum_fix(&img);

		/* K-30: the edit must move the field it owns and the checksum, and nothing else. */
		{
			char got[1024] = "", *g = got;
			size_t i;
			for (i = 0; i < blen; i++)
				if (base[i] != img.bytes[i])
					g += snprintf(g, sizeof got - (size_t)(g - got), "%s%03x:%02x:%02x",
					              g == got ? "" : "|", (unsigned)i, base[i], img.bytes[i]);
			if (!got[0])
				snprintf(got, sizeof got, "none");
			if (strcmp(got, changed) != 0)
				failf("K-30", "%s %s slot %d arg %lld:\n        want %s\n        got  %s", imgp,
				      op, slot, arg, changed, got);
			else
				pass++;
		}
	next:
		free(img.bytes);
		free(base);
	}
	free(vec);
}

/* The Radius M110: a different radio on the same wire protocol.  These assertions exist because
 * every one of them was, at some point, the opposite of what this program assumed. */
static void test_m110(void)
{
	static const struct {
		const char *file, *want, *tag;
		int band, p;
		uint32_t tx_hz;
		int mirrored;
	} R[] = {
		{ "fixtures/m110_cspl_radio.bin",    "m110_cspl", "EZA", 12, 254, 438612500u, 1 },
		{ "fixtures/m110_sel5_radio.bin",    "m110_sel5", "EZ9", 15, 254, 439987500u, 0 },
		{ "fixtures/m110_sel5_2m_radio.bin", "m110_sel5", "EZ9",  7,  80, 144800000u, 0 },
	};
	size_t k;

	for (k = 0; k < sizeof R / sizeof R[0]; k++) {
		size_t len = 0;
		uint8_t *b = (uint8_t *)slurp(R[k].file, &len);
		const mc_model *m;
		mc_image img;
		mc_channel c;
		char note[200];
		uint8_t saved;

		if (!b) {
			failf("K-20", "%s missing", R[k].file);
			continue;
		}
		ok(len == 256, "K-25", "the M110 device returns 256 bytes");

		/* K-20: detected on its own marker and its own checksum rule -- NOT as a damaged
		 * eza_sel5, which is what every one of these did before. */
		m = mc_model_detect(b, len, note, sizeof note);
		if (!m || strcmp(m->name, R[k].want) != 0) {
			failf("K-20", "%s detected as %s, want %s (%s)", R[k].file,
			      m ? m->name : "nothing", R[k].want, note);
			free(b);
			continue;
		}
		ok(1, "K-20", "an M110 codeplug detects as its own model");
		ok(memcmp(b + 7, R[k].tag, 3) == 0, "K-20", "the family tag is at 0x07..0x09");

		img.model = m;
		img.bytes = b;
		img.len = len;
		ok(mc_image_check(&img) == 0, "K-20", "the model fits the image");

		/* K-2: sums to 0x01, not 0xFF, with the byte at 0x0F. */
		ok(m->cksum == 0x0F, "K-2", "the M110 checksum byte is at 0x0F");
		ok(m->cksum_target == 0x01, "K-2", "the M110 checksum target is 0x01");
		ok(mc_checksum_valid(&img), "K-2", "a real M110 codeplug is checksum-valid");
		ok(mc_checksum_total(&img) == 0x01, "K-2", "and the covered bytes really sum to 0x01");

		/* The negative control the 1989 RSS itself gives us: doctored to the MC micro's constant,
		 * it must be INVALID here too. */
		saved = b[0x20];
		b[0x20] = (uint8_t)(b[0x20] + 0xFE); /* moves the sum 0x01 -> 0xFF */
		ok(mc_checksum_total(&img) == 0xFF, "K-2", "the doctored image sums to 0xFF");
		ok(!mc_checksum_valid(&img), "K-2", "an M110 image summing to 0xFF is REJECTED");
		b[0x20] = saved;
		ok(mc_checksum_valid(&img), "K-2", "and restoring it makes it valid again");

		/* K-2 again, from the other side: fixing must not touch 0x000, which on this format is
		 * serial-number byte 0.  Applying the MC micro rule here corrupted the serial. */
		saved = b[0x000];
		b[0x0F] = (uint8_t)(b[0x0F] + 3); /* break it */
		ok(!mc_checksum_valid(&img), "K-2", "a broken M110 checksum is detected");
		mc_checksum_fix(&img);
		ok(mc_checksum_valid(&img), "K-2", "mc_checksum_fix repairs it to the model's target");
		ok(b[0x000] == saved, "K-2", "and leaves 0x000 -- the serial -- alone");

		/* K-20: the band is four bits at the bottom of 0x0A, not three in the middle. */
		ok(mc_band_index(&img) == R[k].band, "K-20", "the M110 band index is bits 0-3 of 0x0A");
		ok(mc_band_p_of(&img) == (unsigned)R[k].p, "K-20", "and maps to the measured P");

		/* K-21: channel 1 decodes to the frequency the radio was programmed with. */
		ok(mc_channel_get(&img, 0, mc_band_p_of(&img), &c) == 0, "K-21", "channel 1 reads");
		ok(c.tx_hz == R[k].tx_hz, "K-21", "and decodes to the programmed TX frequency");

		/* K-25: device size, codeplug size and write extent are three numbers. */
		if (R[k].mirrored) {
			ok(memcmp(b, b + 128, 128) == 0, "K-25",
			   "the CSQ/PL device returns two identical 128-byte copies");
			ok(m->cksum_len == 128, "K-25", "and only the first 128 are covered by the sum");
			ok(mc_write_len(&img) == 128, "K-25", "and only the first 128 are written");
		} else {
			ok(mc_write_len(&img) == 256, "K-25", "the Sel 5 writes all 256");
		}

		/* W-5: neither M110 model has a measured write counter, so nothing may bump one.  On
		 * eza_sel5 that offset is 0x09E, which here is live channel data. */
		ok(m->wcount == 0, "W-5", "no write counter is claimed on the M110");

		/* K-14a: PL is per channel on the CSQ/PL, and absent on the Sel 5. */
		if (strcmp(m->name, "m110_cspl") == 0) {
			static const unsigned EIA[] = {
				670, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035, 1072,
				1109, 1148, 1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1622,
				1679, 1738, 1799, 1862, 1928, 2035, 2107, 2181, 2257, 2336, 2418, 2503
			};
			size_t t;
			int bad = 0;
			uint8_t keep[10];

			ok(mc_pl_per_channel(m), "K-14a", "the CSQ/PL holds PL in the channel record");
			ok(m->pl_ch_enc == 0 && m->pl_ch_dec == 5, "K-14a",
			   "encode at +0 and decode at +5, straddling the TX triplet");
			/* Channel 2 of both CSQ/PL radios is 123.0 Hz on both fields, and the two words
			 * differ because the laws differ: 982 = 123.0 x 7.9844, 7516 = 123.0 x 61.107. */
			ok(mc_channel_pl_enc(&img, 1) == 1230, "K-14a", "channel 2 encodes 123.0 Hz");
			ok(mc_channel_pl_dec(&img, 1) == 1230, "K-14a", "channel 2 decodes 123.0 Hz");
			ok(mc_channel_pl_enc(&img, 0) == 0, "K-14a", "channel 1 has no PL");
			ok(mc_channel_pl_dec(&img, 0) == 0, "K-14a", "and none to decode");
			{
				const uint8_t *r = b + m->chan + m->stride; /* channel 2's record */
				ok(((r[0] << 8) | r[1]) == 982, "K-14a", "the stored encode word is 982");
				ok(((r[5] << 8) | r[6]) == 7516, "K-14a", "the stored decode word is 7516");
				ok(mc_pl_encode_k(1230, MC_PL_K_EZ13) == 982, "K-14a",
				   "  which is round(123.0 x 7.9844)");
				ok(mc_pl_dec_encode(1230) == 7516, "K-14a",
				   "  and round(123.0 x 61.107) -- two laws, not one");
			}
			/* Every EIA tone must survive a round trip through both laws. */
			memcpy(keep, b + m->chan, m->stride);
			for (t = 0; t < sizeof EIA / sizeof EIA[0]; t++) {
				mc_channel_pl_enc_set(&img, 0, EIA[t]);
				mc_channel_pl_dec_set(&img, 0, EIA[t]);
				if (mc_channel_pl_enc(&img, 0) != EIA[t] ||
				    mc_channel_pl_dec(&img, 0) != EIA[t])
					bad++;
			}
			ok(bad == 0, "K-14a", "all 38 EIA tones round-trip through both PL laws");
			/* 118.8 Hz is the ONE tone that separates 7.9844 from 7.9840, so it is the only
			 * evidence that the M110 uses the MCEZ13 constant rather than the EVA one. */
			ok(mc_pl_encode_k(1188, MC_PL_K_EZ13) == 949 &&
			   mc_pl_encode_k(1188, MC_PL_K_EVA) == 948, "K-14a",
			   "118.8 Hz is the only tone separating 7.9844 from 7.9840");
			memcpy(b + m->chan, keep, m->stride);
		} else {
			ok(!mc_pl_per_channel(m), "K-14a", "the Sel 5 has no PL at all");
			ok(mc_channel_pl_enc(&img, 0) == 0, "K-14a", "  and reports none");
		}
		free(b);
	}

	/* K-20: the MC micro models must not claim M110 bytes, and vice versa.  Before the marker
	 * test, a 256-byte M110 image reached `eza_sel5' and was reported as merely damaged. */
	{
		size_t len = 0;
		uint8_t *b = (uint8_t *)slurp("fixtures/eza9_radio.bin", &len);
		char note[200];
		const mc_model *m;
		if (b) {
			m = mc_model_detect(b, len, note, sizeof note);
			ok(m && strcmp(m->name, "eza_sel5") == 0, "K-20",
			   "a real MC micro EZA 9 still detects as eza_sel5");
			/* and it carries no M110 marker */
			ok(memcmp(b + 7, "EZA", 3) != 0 && memcmp(b + 7, "EZ9", 3) != 0, "K-20",
			   "an MC micro codeplug carries no M110 family tag");
			free(b);
		}
	}
}

int main(int argc, char **argv)
{
	static const char *CODEPLUGS[] = {
		"testdata/codeplug/mcmicr70.vec",
		"testdata/codeplug/mcmicr2m.vec",
		"testdata/codeplug/eva9_real.vec",
		"testdata/codeplug/ev9_default.vec",
		"testdata/codeplug/eza9_default_band2.vec",
		"testdata/codeplug/eza9_programmed.vec",
		"testdata/codeplug/eza9_radio.vec",
		"testdata/codeplug/ez13_default_band2.vec",
	};
	size_t i;

	if (argc > 1)
		ROOT = argv[1];
	for (i = 0; i < sizeof CODEPLUGS / sizeof CODEPLUGS[0]; i++)
		test_codeplug(CODEPLUGS[i]);
	test_detect_ident();
	test_m110();
	test_freq();
	test_edits();
	test_pl();
	test_aak();
	test_parity();

	printf("\n%d passed, %d FAILED\n", pass, fail);
	return fail ? 1 : 0;
}
