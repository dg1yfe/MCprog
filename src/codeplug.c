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

/* ---- bounds ---------------------------------------------------------------------------------- */

size_t mc_image_check(const mc_image *img)
{
	const mc_model *m = img->model;
	size_t need = 0, x;

	/* The nominal device size is a floor, not just the accessors' high-water mark.  They differ on
	 * the M110 CSQ/PL, which declares 256 (what the device returns) but addresses only the first
	 * 128 (the codeplug; the rest is a mirror -- K-25).  Detection and the write path both compare
	 * `img->len' against `size', so an image shorter than that is unusable however few bytes the
	 * accessors happen to touch. */
	need = m->size;

	/* the highest byte any accessor can touch, per model */
	x = (size_t)m->cksum + 1;                                     if (x > need) need = x;
	x = (size_t)m->band + 1;                                      if (x > need) need = x;
	x = (size_t)m->refdiv + 4;                                    if (x > need) need = x;
	x = (size_t)m->chan + (size_t)m->nchan * m->stride;           if (x > need) need = x;
	if (m->cksum_len) {
		x = m->cksum_len;                                         if (x > need) need = x;
	}
	if (m->pl_list) {
		x = (size_t)m->pl_list + (size_t)m->pl_max * 2;            if (x > need) need = x;
		x = (size_t)m->pl_tone + 2;                                if (x > need) need = x;
	}
	if (m->pl_dec) {
		x = (size_t)m->pl_dec + (size_t)m->pl_max * 2;             if (x > need) need = x;
	}
	if (m->pl_count) { x = (size_t)m->pl_count + 1;                if (x > need) need = x; }
	if (m->pl_mode)  { x = (size_t)m->pl_mode + 1;                 if (x > need) need = x; }
	return img->len >= need ? 0 : need;
}

/* ---- checksum, K-2 -------------------------------------------------------------------------- */

uint8_t mc_checksum_stored(const mc_image *img)
{
	return img->bytes[img->model->cksum];
}

uint8_t mc_checksum_total(const mc_image *img)
{
	/* K-2: the covered range is per-model.  MCEZ13 sums all but its last two bytes; every other
	 * model sums the whole device. */
	size_t n = img->model->cksum_len ? img->model->cksum_len : img->len;
	uint8_t sum = 0;
	size_t i;
	if (n > img->len)
		n = img->len;
	for (i = 0; i < n; i++)
		sum = (uint8_t)(sum + img->bytes[i]);
	return sum;
}

int mc_checksum_valid(const mc_image *img)
{
	return mc_checksum_total(img) == img->model->cksum_target;
}

uint8_t mc_checksum_fix(mc_image *img)
{
	uint8_t *cell = &img->bytes[img->model->cksum];
	uint8_t want = img->model->cksum_target;
	/* Solve for the stored byte: it is part of the sum, so back it out first.  The target is
	 * per-model -- 0xFF on the MC micro, 0x01 on the M110 -- and getting it wrong does not merely
	 * leave the image invalid, it overwrites a live byte with a plausible-looking wrong one. */
	*cell = (uint8_t)(*cell - (uint8_t)(mc_checksum_total(img) - want));
	return *cell;
}

size_t mc_write_len(const mc_image *img)
{
	size_t n = img->model->write_len ? img->model->write_len : img->len;
	return n > img->len ? img->len : n;
}

/* ---- band, K-10 ----------------------------------------------------------------------------- */

int mc_band_index(const mc_image *img)
{
	const mc_model *m = img->model;
	return (img->bytes[m->band] >> m->band_shift) & m->band_mask;
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

unsigned mc_band_p_of(const mc_image *img)
{
	const mc_model *m = img->model;
	int i = mc_band_index(img);

	if (!m->band_p)
		return mc_band_p(i);
	/* A model-supplied table.  Entries are 0 where the band exists but its P has never been
	 * measured, and 0 reads downstream as "not computable" -- which is the honest answer. */
	return (i >= 0 && i < m->band_n) ? m->band_p[i] : 0;
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
	unsigned p = mc_band_p_of(img);
	uint8_t *f;
	mc_channel before;
	int was_empty;

	if (slot0 < 0 || slot0 >= img->model->nchan)
		return -1;
	if (p == 0)
		return -1; /* K-10: band unprogrammed, nothing is computable */
	mc_channel_get(img, slot0, p, &before);
	was_empty = before.state == MC_CH_EMPTY;
	if (dir == MC_RX) {
		if (hz < MC_IF_HZ)
			return -1;
		hz -= MC_IF_HZ; /* the field holds the local oscillator */
	}
	f = half_ptr(img, slot0, dir);
	/* The existing b0 supplies the flag bits, which this must not disturb (K-22). */
	if (mc_freq_encode(hz, p, mc_step_hz(img), f[0], f) != 0)
		return -1;
	/* K-24a.  An empty slot's flag bits are leftovers, not settings, and on MCEZ13 clock shift is
	 * stored inverted -- so a zeroed record reads as clock shift ON.  Programming such a channel
	 * would silently inherit that.  Default it off; an already-programmed channel is left alone. */
	if (was_empty) {
		const mc_flag *cs = mc_flag_by_name(img->model, "clock_shift");
		if (cs)
			mc_flag_set(img, slot0, cs, 0);
	}
	return 0;
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

uint16_t mc_pl_encode_k(unsigned dhz, unsigned k)
{
	return (uint16_t)(((unsigned long)k * dhz + 50000u) / 100000u);
}

unsigned mc_pl_decode_k(uint16_t word, unsigned k)
{
	size_t i;
	if (!word)
		return 0;
	/* Snap: the storage is lossy, so 707 means the 88.5 the operator typed, not 88.55. */
	for (i = 1; i < sizeof PL_STD / sizeof PL_STD[0]; i++)
		if (mc_pl_encode_k(PL_STD[i], k) == word)
			return PL_STD[i];
	return (unsigned)(((unsigned long)word * 100000u + k / 2) / k);
}

uint16_t mc_pl_encode(unsigned dhz)
{
	return mc_pl_encode_k(dhz, MC_PL_K_EVA);
}

unsigned mc_pl_decode(uint16_t word)
{
	return mc_pl_decode_k(word, MC_PL_K_EVA);
}

/* ---- auto-acknowledge delay, K-15 ------------------------------------------------------------
 * The count is 1/64 s.  Integer arithmetic throughout, with the halves added by hand, so that the
 * result does not depend on a floating-point mode: 15.625 = 15625/1000, and 1/64 s exactly.
 */
int mc_aak_supported(const mc_model *m)
{
	return m->aak != 0;
}

unsigned mc_aak_encode_ms(unsigned ms)
{
	return (ms * 64u + 500u) / 1000u; /* round(ms / 15.625) */
}

unsigned mc_aak_decode_count(unsigned count)
{
	return (count * 15625u + 500u) / 1000u; /* round(count x 15.625) */
}

unsigned mc_aak_get_ms(const mc_image *img)
{
	unsigned count;

	if (!mc_aak_supported(img->model) || img->len <= img->model->aak)
		return 0;
	count = img->bytes[img->model->aak] & 0x7Fu; /* bit 7 is not part of the value */
	return count ? mc_aak_decode_count(count) : 0;
}

int mc_aak_set_ms(mc_image *img, unsigned ms)
{
	unsigned count;
	uint8_t *b;

	if (!mc_aak_supported(img->model) || img->len <= img->model->aak)
		return -1;
	if (ms < MC_AAK_MIN_MS || ms > MC_AAK_MAX_MS)
		return -1; /* U-3: refuse, never clamp */
	count = mc_aak_encode_ms(ms);
	if (count < 1 || count > 127)
		return -1; /* unreachable from the range check above; kept so the law is enforced here */
	b = &img->bytes[img->model->aak];
	*b = (uint8_t)((*b & 0x80u) | count); /* K-30: bit 7 belongs to whoever set it */
	return 0;
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

/* ---- PL in the channel record, K-14a --------------------------------------------------------- */

int mc_pl_per_channel(const mc_model *m)
{
	return m->pl_ch_enc != MC_PL_NONE || m->pl_ch_dec != MC_PL_NONE;
}

/* The word at `within' bytes into slot `slot0's record, or NULL if this model has no such field. */
static uint8_t *pl_ch_ptr(const mc_image *img, int slot0, uint8_t within)
{
	const mc_model *m = img->model;
	size_t o;

	if (within == MC_PL_NONE || slot0 < 0 || slot0 >= m->nchan)
		return NULL;
	o = (size_t)m->chan + (size_t)slot0 * m->stride + within;
	if (o + 1 >= img->len)
		return NULL;
	return img->bytes + o;
}

unsigned mc_channel_pl_enc(const mc_image *img, int slot0)
{
	const uint8_t *p = pl_ch_ptr(img, slot0, img->model->pl_ch_enc);
	return p ? mc_pl_decode_k((uint16_t)((p[0] << 8) | p[1]), MC_PL_K_EZ13) : 0;
}

unsigned mc_channel_pl_dec(const mc_image *img, int slot0)
{
	const uint8_t *p = pl_ch_ptr(img, slot0, img->model->pl_ch_dec);
	return p ? mc_pl_dec_decode((uint16_t)((p[0] << 8) | p[1])) : 0;
}

int mc_channel_pl_enc_set(mc_image *img, int slot0, unsigned dhz)
{
	uint8_t *p = pl_ch_ptr(img, slot0, img->model->pl_ch_enc);
	uint16_t w;

	if (!p)
		return -1;
	w = dhz ? mc_pl_encode_k(dhz, MC_PL_K_EZ13) : 0;
	p[0] = (uint8_t)(w >> 8);
	p[1] = (uint8_t)w;
	return 0;
}

int mc_channel_pl_dec_set(mc_image *img, int slot0, unsigned dhz)
{
	uint8_t *p = pl_ch_ptr(img, slot0, img->model->pl_ch_dec);
	uint16_t w;

	if (!p)
		return -1;
	w = dhz ? mc_pl_dec_encode(dhz) : 0;
	p[0] = (uint8_t)(w >> 8);
	p[1] = (uint8_t)w;
	return 0;
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
	return mc_pl_decode_k((uint16_t)((img->bytes[o] << 8) | img->bytes[o + 1]),
			      img->model->pl_k);
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
	w = dhz ? mc_pl_encode_k(dhz, img->model->pl_k) : 0;
	o = pl_slot(img, i);
	img->bytes[o] = (uint8_t)(w >> 8);
	img->bytes[o + 1] = (uint8_t)(w & 0xFF);
	return 0;
}

/* ---- timers, K-16 -----------------------------------------------------------------------------
 * Measured on MCEV_56 by mutating each byte and re-rendering the original's timers sub-screen; see
 * ../doc/EEPROM_MAP.md.  Two of the twelve are not the plain x10 ms the rest are: the synthesiser
 * lock time is round(n x 5/6) + 10 ms -- two points look exactly like n-4 and a third refutes it --
 * and the TX time-out masks off bit 15 and carries a +4 s offset that the rekey timer beside it
 * does not.
 */
size_t mc_timer_count(const mc_model *m)
{
	return m->timers ? m->ntimers : 0;
}

const mc_timer *mc_timer_at(const mc_model *m, size_t i)
{
	return (m->timers && i < m->ntimers) ? &m->timers[i] : NULL;
}

unsigned mc_timer_decode(const mc_timer *t, unsigned raw)
{
	unsigned v = raw & t->mask;
	/* round(v * 10 / den) -- the original divides for real and calls the runtime's Round entry */
	return (v * 10u + t->den / 2u) / t->den + t->add_ms;
}

static unsigned timer_raw(const mc_image *img, const mc_timer *t)
{
	if (t->width == 1)
		return img->bytes[t->off];
	return (unsigned)(img->bytes[t->off] << 8) | img->bytes[t->off + 1];
}

unsigned mc_timer_get_ms(const mc_image *img, size_t i)
{
	const mc_timer *t = mc_timer_at(img->model, i);
	return t ? mc_timer_decode(t, timer_raw(img, t)) : 0;
}

int mc_timer_set_ms(mc_image *img, size_t i, unsigned ms)
{
	const mc_timer *t = mc_timer_at(img->model, i);
	unsigned old, v, raw;

	if (!t || ms < t->add_ms)
		return -1;
	/* Invert, then insist the value round-trips: that is what refuses everything the law cannot
	 * spell, without a separate range test per field. */
	v = ((ms - t->add_ms) * t->den + 5u) / 10u;
	if (v > t->mask || mc_timer_decode(t, v) != ms)
		return -1;

	old = timer_raw(img, t);
	raw = (old & ~(unsigned)t->mask) | v;   /* every bit outside the field stays as found */
	if (t->width == 1) {
		img->bytes[t->off] = (uint8_t)raw;
	} else {
		img->bytes[t->off] = (uint8_t)(raw >> 8);
		img->bytes[t->off + 1] = (uint8_t)(raw & 0xFF);
	}
	return 0;
}

/* ---- the write counter (W-5) ---------------------------------------------------------------- */

uint8_t mc_write_counter_next(const mc_model *m, uint8_t cur)
{
	unsigned lo = (unsigned)(cur & 0x0Fu) + 1u;
	uint8_t v = lo > 0x0Fu ? (uint8_t)((cur | 0x10u) & 0xF0u)   /* wrap: clear the count, SET b4 */
	                       : (uint8_t)((cur & 0xF0u) | lo);
	return (uint8_t)(v & ~m->wcount_clr);
}

int mc_write_counter_bump(mc_image *img)
{
	const mc_model *m = img->model;

	if (!m->wcount || (size_t)m->wcount >= img->len)
		return -1;
	img->bytes[m->wcount] = mc_write_counter_next(m, img->bytes[m->wcount]);
	return 0;
}

