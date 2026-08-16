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
/* Emit the conformance format of testdata/codeplug/[*].vec.
 *
 * The CLI and the test suite both call this, so what the tests pin is exactly what the tool
 * prints.  The `#` comment lines in the .vec files are generator provenance and are not
 * reproduced here; the comparison skips them.
 */
#include <stdio.h>
#include "mc/codeplug.h"

static void hex(char *dst, const uint8_t *b, size_t n)
{
	static const char D[] = "0123456789abcdef";
	size_t i;
	for (i = 0; i < n; i++) {
		dst[i * 2] = D[b[i] >> 4];
		dst[i * 2 + 1] = D[b[i] & 15];
	}
	dst[n * 2] = 0;
}

static void dump_pl(FILE *f, const mc_image *img);
static void dump_channels(FILE *f, const mc_image *img, unsigned p, int live, int term);

void mc_dump_vec(FILE *f, const mc_image *img, const char *path)
{
	const mc_model *m = img->model;
	int band = mc_band_index(img);
	unsigned p = mc_band_p(band);
	int term = 0, live = mc_channel_count(img, &term);
	char pbuf[16], tbuf[16];

	fprintf(f, "IMG    %s\n", path);
	fprintf(f, "MODEL  %s size=%d\n", m->name, (int)img->len);
	fprintf(f, "SUM    stored=0x%02x total=0x%02x valid=%d\n", mc_checksum_stored(img),
	        mc_checksum_total(img), mc_checksum_valid(img));

	if (p)
		snprintf(pbuf, sizeof pbuf, "%u", p);
	else
		snprintf(pbuf, sizeof pbuf, "none");
	fprintf(f, "BAND   index=%d p=%s raster=%d\n", band, pbuf, mc_band_raster(img));
	fprintf(f, "REFDIV %04x %04x\n", mc_refdiv(img, 0), mc_refdiv(img, 1));

	if (term)
		snprintf(tbuf, sizeof tbuf, "%d", term);
	else
		snprintf(tbuf, sizeof tbuf, "none");
	fprintf(f, "CHANS  terminated=%d slots=%d terminator=%s\n", live, m->nchan, tbuf);

	if (!p)
		/* K-10: band 7 is unprogrammed.  This is a question for the user, not an error. */
		fprintf(f, "NOTE   band unprogrammed -- frequencies are not computable (spec K-10)\n");
	else
		dump_channels(f, img, p, live, term);
	dump_pl(f, img);
}

/* K-14.  With PL off the count and list bytes hold unrelated data, so rendering them as tones
 * would repeat the mistake K-24 forbids for unprogrammed channels. */
static void dump_pl(FILE *f, const mc_image *img)
{
	int i, n;
	if (!mc_pl_supported(img->model))
		return;
	switch (mc_pl_get_mode(img)) {
	case MC_PL_OFF:
		fprintf(f, "PL     mode=off\n");
		return;
	case MC_PL_SINGLE:
		fprintf(f, "PL     mode=single tone=%u.%u\n", mc_pl_get_tone(img, 0) / 10,
		        mc_pl_get_tone(img, 0) % 10);
		return;
	default:
		break;
	}
	n = mc_pl_get_count(img);
	fprintf(f, "PL     mode=selectable count=%d\n", n);
	fprintf(f, "PLLIST");
	for (i = 0; i < n; i++) {
		unsigned t = mc_pl_get_tone(img, i);
		if (t)
			fprintf(f, " %u.%u", t / 10, t % 10);
		else
			fprintf(f, " -");
	}
	fputc('\n', f);
}

static void dump_channels(FILE *f, const mc_image *img, unsigned p, int live, int term)
{
	const mc_model *m = img->model;
	char tx[8], rx[8], raw[40];
	int i;
	{

	for (i = 0; i < live; i++) {
		mc_channel c;
		char num[4];
		mc_channel_get(img, i, p, &c);
		if (m->numbered)
			snprintf(num, sizeof num, "%02x", c.num);
		else
			snprintf(num, sizeof num, "--");
		hex(tx, c.txraw, 3);
		hex(rx, c.rxraw, 3);
		fprintf(f, "CHAN   %-2d num=%s tx=%u rx=%u txraw=%s rxraw=%s canon=%d state=%s\n", c.slot,
		        num, c.tx_hz, c.rx_hz, tx, rx, c.canonical,
		        c.state == MC_CH_EMPTY ? "empty" : "prog");
	}
	/* K-23: everything from the terminator on is stale and must survive a write untouched. */
	if (term)
		for (i = term - 1; i < m->nchan; i++) {
			mc_channel c;
			mc_channel_get(img, i, p, &c);
			hex(raw, c.raw, m->stride);
			fprintf(f, "STALE  %-2d raw=%s\n", c.slot, raw);
		}
	}
}
