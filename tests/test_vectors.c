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
	char *vec = slurp("testdata/pl/pl.vec", NULL), *line, *save;
	if (!vec)
		return;
	for (line = strtok_r(vec, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char mname[64];
		unsigned idx, dhz, word, tone, list, cnt, mode, dec, mx;

		if (sscanf(line, "TONE idx=%u dhz=%u word=%x", &idx, &dhz, &word) == 3) {
			ok(mc_pl_standard(idx) == dhz, "K-14", "the standard tone list matches");
			ok(mc_pl_encode(dhz) == word, "K-14", "and each entry encodes as expected");
		} else if (sscanf(line, "PLENC dhz=%u word=%x", &dhz, &word) == 2) {
			ok(mc_pl_encode(dhz) == word, "K-14", "encode round(7.984 x f)");
		} else if (sscanf(line, "PLDEC word=%x dhz=%u", &word, &dhz) == 2) {
			ok(mc_pl_decode((uint16_t)word) == dhz, "K-14",
			   "decode, snapped to the standard list where one matches");
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
			mc_pl_set_count(&im, 99);
			ok(mc_pl_get_count(&im) == im.model->pl_max, "K-14",
			   "an out-of-range count is clamped to the model's maximum");
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

/* ---- edits (K-11, K-22, K-30, U-3) ---------------------------------------------------------- */

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

int main(int argc, char **argv)
{
	static const char *CODEPLUGS[] = {
		"testdata/codeplug/mcmicr70.vec",
		"testdata/codeplug/mcmicr2m.vec",
		"testdata/codeplug/eva9_real.vec",
		"testdata/codeplug/ev9_default.vec",
		"testdata/codeplug/eza9_default_band2.vec",
		"testdata/codeplug/eza9_programmed.vec",
		"testdata/codeplug/ez13_default_band2.vec",
	};
	size_t i;

	if (argc > 1)
		ROOT = argv[1];
	for (i = 0; i < sizeof CODEPLUGS / sizeof CODEPLUGS[0]; i++)
		test_codeplug(CODEPLUGS[i]);
	test_freq();
	test_edits();
	test_pl();
	test_parity();

	printf("\n%d passed, %d FAILED\n", pass, fail);
	return fail ? 1 : 0;
}
