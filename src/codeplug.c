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
/* Codeplug decoding -- spec.md section 6. */
#include <string.h>
#include "mc/codeplug.h"

/* ---- checksum, K-2 -------------------------------------------------------------------------- */

uint8_t mc_checksum_stored(const mc_image *img)
{
	return img->bytes[img->model->cksum];
}

uint8_t mc_checksum_total(const mc_image *img)
{
	uint8_t sum = 0;
	size_t i;
	for (i = 0; i < img->len; i++)
		sum = (uint8_t)(sum + img->bytes[i]);
	return sum;
}

int mc_checksum_valid(const mc_image *img)
{
	return mc_checksum_total(img) == 0xFF;
}

uint8_t mc_checksum_fix(mc_image *img)
{
	uint8_t *cell = &img->bytes[img->model->cksum];
	/* Solve for the stored byte: it is part of the sum, so back it out first. */
	*cell = (uint8_t)(*cell - (uint8_t)(mc_checksum_total(img) - 0xFF));
	return *cell;
}

/* ---- band, K-10 ----------------------------------------------------------------------------- */

int mc_band_index(const mc_image *img)
{
	return (img->bytes[img->model->band] >> 4) & 7;
}

int mc_band_raster(const mc_image *img)
{
	return (img->bytes[img->model->band] >> 7) & 1;
}

unsigned mc_band_p(int band_index)
{
	switch (band_index) {
	case 1: return 80;
	case 2: return 80;
	case 3: return 128;
	case 4: return 254;
	default: return 0; /* 7 = unprogrammed; anything else is unknown */
	}
}

uint16_t mc_refdiv(const mc_image *img, int which)
{
	size_t o = (size_t)img->model->refdiv + (size_t)(which ? 2 : 0);
	return (uint16_t)((img->bytes[o] << 8) | img->bytes[o + 1]);
}

/* ---- frequency codec, K-10 / K-11 ----------------------------------------------------------- */

uint32_t mc_freq_decode(const uint8_t raw[3], unsigned p)
{
	uint32_t coarse = (uint32_t)((raw[0] & 3) << 8 | raw[1]);
	uint32_t step = (raw[0] & 4) ? 3125u : 2500u;
	return (coarse * p + raw[2]) * step;
}

int mc_freq_is_canonical(const uint8_t raw[3], unsigned p)
{
	return p != 0 && raw[2] < p;
}

int mc_freq_encode(uint32_t hz, unsigned p, unsigned step, uint8_t flags, uint8_t out[3])
{
	uint32_t units, coarse, rem;

	if (p == 0 || step == 0 || hz % step != 0)
		return -1;
	units = hz / step;
	coarse = units / p;
	rem = units % p;
	if (coarse > 1023)
		return -1;
	/* Preserve b0 bits 3-7 (per-channel option bits, K-22); bits 0-1 are the coarse high bits
	 * and bit 2 selects the raster. */
	out[0] = (uint8_t)((flags & 0xF8) | ((coarse >> 8) & 3) | (step == 3125 ? 4 : 0));
	out[1] = (uint8_t)(coarse & 0xFF);
	out[2] = (uint8_t)rem; /* < p by construction: canonical (K-11) */
	return 0;
}

/* ---- channels, K-21 / K-23 / K-24 ----------------------------------------------------------- */

int mc_channel_count(const mc_image *img, int *terminator)
{
	const mc_model *m = img->model;
	int i;

	if (terminator)
		*terminator = 0;
	if (!m->numbered)
		return m->nchan; /* no number byte, so nothing to terminate on */
	for (i = 0; i < m->nchan; i++) {
		if (img->bytes[m->chan + (size_t)i * m->stride] == 0xFF) {
			if (terminator)
				*terminator = i + 1; /* 1-based slot holding the terminator */
			return i;
		}
	}
	return m->nchan;
}

int mc_channel_get(const mc_image *img, int slot0, unsigned p, mc_channel *out)
{
	const mc_model *m = img->model;
	const uint8_t *rec;
	int live, term;

	if (slot0 < 0 || slot0 >= m->nchan)
		return -1;
	rec = &img->bytes[m->chan + (size_t)slot0 * m->stride];
	memset(out, 0, sizeof *out);
	out->slot = slot0 + 1;
	memcpy(out->raw, rec, m->stride);
	memcpy(out->txraw, rec + m->tx, 3);
	memcpy(out->rxraw, rec + m->rx, 3);
	out->num = m->numbered ? rec[0] : 0;

	live = mc_channel_count(img, &term);
	if (slot0 >= live) {
		out->state = MC_CH_STALE; /* K-23: leftover, must be written back unchanged */
		return 0;
	}
	if (p) {
		out->tx_hz = mc_freq_decode(out->txraw, p);
		out->rx_hz = mc_freq_decode(out->rxraw, p) + MC_IF_HZ;
		out->canonical = mc_freq_is_canonical(out->txraw, p) &&
		                 mc_freq_is_canonical(out->rxraw, p);
		/* K-24: a slot can be allocated but unprogrammed.  Never render that as 0.00000 MHz. */
		if (out->tx_hz == 0 && mc_freq_decode(out->rxraw, p) == 0)
			out->state = MC_CH_EMPTY;
	}
	return 0;
}

/* ---- software parity, P-2 ------------------------------------------------------------------- */

static int odd_parity(uint8_t b)
{
	int n = 0;
	while (b) {
		n ^= b & 1;
		b >>= 1;
	}
	return n; /* 1 when the popcount is odd */
}

uint8_t mc_parity_tx(uint8_t b)
{
	b &= 0x7F;
	/* odd parity: bit 7 makes the total number of set bits odd */
	return (uint8_t)(b | (odd_parity(b) ? 0 : 0x80));
}

int mc_parity_rx(uint8_t b, uint8_t *out)
{
	if (out)
		*out = b & 0x7F;
	return odd_parity(b) ? 0 : -1;
}

/* ---- editing, K-11 / K-22 / K-30 ------------------------------------------------------------ */

unsigned mc_step_hz(const mc_image *img)
{
	return mc_band_raster(img) ? 3125u : 2500u;
}

static uint8_t *half_ptr(mc_image *img, int slot0, mc_dir dir)
{
	const mc_model *m = img->model;
	return &img->bytes[m->chan + (size_t)slot0 * m->stride + (dir == MC_TX ? m->tx : m->rx)];
}

int mc_channel_set_freq(mc_image *img, int slot0, mc_dir dir, uint32_t hz)
{
	unsigned p = mc_band_p(mc_band_index(img));
	uint8_t *f;

	if (slot0 < 0 || slot0 >= img->model->nchan)
		return -1;
	if (p == 0)
		return -1; /* K-10: band unprogrammed, nothing is computable */
	if (dir == MC_RX) {
		if (hz < MC_IF_HZ)
			return -1;
		hz -= MC_IF_HZ; /* the field holds the local oscillator */
	}
	f = half_ptr(img, slot0, dir);
	/* The existing b0 supplies the flag bits, which this must not disturb (K-22). */
	return mc_freq_encode(hz, p, mc_step_hz(img), f[0], f);
}

int mc_flag_get(const mc_image *img, int slot0, const mc_flag *fl)
{
	const mc_model *m = img->model;
	size_t off = m->chan + (size_t)slot0 * m->stride +
	             (fl->half == MC_HALF_RX ? m->rx : m->tx);
	int on = (img->bytes[off] >> fl->bit) & 1;
	return fl->inverted ? !on : on;
}

void mc_flag_set(mc_image *img, int slot0, const mc_flag *fl, int on)
{
	const mc_model *m = img->model;
	size_t base = m->chan + (size_t)slot0 * m->stride;
	int store = fl->inverted ? !on : on;
	uint8_t offs[2];
	int n = 0, i;

	/* The editor writes most flags into BOTH halves of the record; a few live in one only. */
	if (fl->half == MC_HALF_BOTH || fl->half == MC_HALF_TX)
		offs[n++] = m->tx;
	if (fl->half == MC_HALF_BOTH || fl->half == MC_HALF_RX)
		offs[n++] = m->rx;
	for (i = 0; i < n; i++) {
		size_t off = base + offs[i];
		if (store)
			img->bytes[off] |= (uint8_t)(1u << fl->bit);
		else
			img->bytes[off] &= (uint8_t)~(1u << fl->bit);
	}
}

/* ---- PL / CTCSS, K-14 ----------------------------------------------------------------------- */

/* The list the original software carries (EVA image, CS:0x3436): 40 little-endian words in tenths
 * of a Hz, index 0 = 0.0 meaning no PL.  Held here because decoding snaps to it. */
static const unsigned PL_STD[] = {
	0, 670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 974, 1000, 1035, 1072, 1109,
	1148, 1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1622, 1679, 1738, 1799,
	1862, 1928, 2035, 2065, 2107, 2181, 2257, 2336, 2418, 2503
};

unsigned mc_pl_standard(size_t i)
{
	return i < sizeof PL_STD / sizeof PL_STD[0] ? PL_STD[i] : 0;
}

size_t mc_pl_standard_count(void)
{
	return sizeof PL_STD / sizeof PL_STD[0];
}

uint16_t mc_pl_encode(unsigned dhz)
{
	return (uint16_t)((7984u * dhz + 5000u) / 10000u);
}

unsigned mc_pl_decode(uint16_t word)
{
	size_t i;
	if (!word)
		return 0;
	/* Snap: the storage is lossy, so 707 means the 88.5 the operator typed, not 88.55. */
	for (i = 1; i < sizeof PL_STD / sizeof PL_STD[0]; i++)
		if (mc_pl_encode(PL_STD[i]) == word)
			return PL_STD[i];
	return ((unsigned)word * 10000u + 3992u) / 7984u;
}

int mc_pl_supported(const mc_model *m)
{
	return m->pl_list != 0;
}

int mc_pl_has_decoder(const mc_model *m)
{
	return m->pl_dec != 0;
}

/* The decoder's law is its own: round(61.107 x f_Hz), against the encoder's round(7.984 x f_Hz). */
uint16_t mc_pl_dec_encode(unsigned dhz)
{
	return (uint16_t)((61107u * dhz + 5000u) / 10000u);
}

unsigned mc_pl_dec_decode(uint16_t word)
{
	size_t i;
	if (!word)
		return 0;
	for (i = 1; i < sizeof PL_STD / sizeof PL_STD[0]; i++)
		if (mc_pl_dec_encode(PL_STD[i]) == word)
			return PL_STD[i];
	return ((unsigned)word * 10000u + 30553u) / 61107u;
}

unsigned mc_pl_dec_get(const mc_image *img, int i)
{
	size_t o;
	if (!mc_pl_has_decoder(img->model) || i < 0 || i >= img->model->pl_max)
		return 0;
	o = img->model->pl_dec + (size_t)i * 2;
	return mc_pl_dec_decode((uint16_t)((img->bytes[o] << 8) | img->bytes[o + 1]));
}

int mc_pl_dec_set(mc_image *img, int i, unsigned dhz)
{
	size_t o;
	uint16_t w;
	if (!mc_pl_has_decoder(img->model) || i < 0 || i >= img->model->pl_max)
		return -1;
	if (dhz && (dhz < 670 || dhz > 2510))
		return -1;
	w = dhz ? mc_pl_dec_encode(dhz) : 0;
	o = img->model->pl_dec + (size_t)i * 2;
	img->bytes[o] = (uint8_t)(w >> 8);
	img->bytes[o + 1] = (uint8_t)(w & 0xFF);
	return 0;
}

mc_pl_mode mc_pl_get_mode(const mc_image *img)
{
	uint8_t v;
	if (!mc_pl_supported(img->model))
		return MC_PL_OFF;
	if (!img->model->pl_mode)
		return MC_PL_TABLE; /* MCEZ13: the tables are simply there */
	v = (uint8_t)(img->bytes[img->model->pl_mode] & 0xF0);
	if (v == 0x60)
		return MC_PL_SINGLE;
	if (v == 0xE0)
		return MC_PL_SELECTABLE;
	return MC_PL_OFF;
}

void mc_pl_set_mode(mc_image *img, mc_pl_mode mode)
{
	uint8_t *p;
	if (!mc_pl_supported(img->model) || !img->model->pl_mode)
		return;
	p = &img->bytes[img->model->pl_mode];
	/* Only the high nibble is ours; the low nibble is not understood, so it is preserved (K-30). */
	*p = (uint8_t)((*p & 0x0F) |
	               (mode == MC_PL_SINGLE ? 0x60 : mode == MC_PL_SELECTABLE ? 0xE0 : 0x00));
}

int mc_pl_get_count(const mc_image *img)
{
	int n;
	mc_pl_mode m = mc_pl_get_mode(img);
	if (m == MC_PL_TABLE)
		return img->model->pl_max;
	if (m != MC_PL_SELECTABLE)
		return 0;
	n = img->bytes[img->model->pl_count] >> 4;
	return n > img->model->pl_max ? img->model->pl_max : n;
}

void mc_pl_set_count(mc_image *img, int n)
{
	uint8_t *p;
	if (!mc_pl_supported(img->model) || !img->model->pl_count)
		return;
	if (n < 1)
		n = 1;
	if (n > img->model->pl_max)
		n = img->model->pl_max;
	p = &img->bytes[img->model->pl_count];
	*p = (uint8_t)((*p & 0x0F) | (n << 4)); /* low nibble is the selectable-lockout marker */
}

static size_t pl_slot(const mc_image *img, int i)
{
	const mc_model *m = img->model;
	return mc_pl_get_mode(img) == MC_PL_SINGLE ? m->pl_tone
	                                           : m->pl_list + (size_t)i * 2;
	/* MC_PL_TABLE and MC_PL_SELECTABLE both index the list */
}

unsigned mc_pl_get_tone(const mc_image *img, int i)
{
	size_t o;
	if (!mc_pl_supported(img->model) || i < 0 || i >= img->model->pl_max)
		return 0;
	o = pl_slot(img, i);
	return mc_pl_decode((uint16_t)((img->bytes[o] << 8) | img->bytes[o + 1]));
}

int mc_pl_set_tone(mc_image *img, int i, unsigned dhz)
{
	size_t o;
	uint16_t w;
	if (!mc_pl_supported(img->model) || i < 0 || i >= img->model->pl_max)
		return -1;
	/* 0 disables, as the original's prompt says; otherwise the radio's stated range. */
	if (dhz && (dhz < 670 || dhz > 2510))
		return -1;
	w = dhz ? mc_pl_encode(dhz) : 0;
	o = pl_slot(img, i);
	img->bytes[o] = (uint8_t)(w >> 8);
	img->bytes[o + 1] = (uint8_t)(w & 0xFF);
	return 0;
}
