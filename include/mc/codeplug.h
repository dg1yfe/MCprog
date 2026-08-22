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
/* MC micro codeplug decoding -- see ../../spec.md, section 6 (K-n requirements).
 *
 * This layer is pure byte manipulation over an in-memory EEPROM image.  It does no I/O, knows
 * nothing about the serial link and nothing about the terminal, so it is testable headlessly and
 * is shared unchanged between the file editor and the radio programmer.
 */
#ifndef MC_CODEPLUG_H
#define MC_CODEPLUG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- channel flag bits (K-22) ---------------------------------------------------------------
 * NOT portable between models: bit 3 is clock shift on the EVA and auto-acknowledge on the EZA 9,
 * and MCEZ13 has almost none of them because PL lives in tables instead.  The editor writes flags
 * into BOTH halves of a channel record unless `half` says otherwise.
 */
typedef enum { MC_HALF_BOTH = 0, MC_HALF_TX, MC_HALF_RX } mc_half;

typedef struct {
	const char *name;
	uint8_t bit;
	mc_half half;
	uint8_t inverted;  /* stored sense is reversed (MCEZ13 clock shift: 1 = off) */
	char provenance;   /* 'C' measured against the original, 'S' disassembly only */
} mc_flag;

/* ---- model descriptor (K-20) ---------------------------------------------------------------
 * These fields genuinely differ between models, so they are data rather than assumptions -- the
 * checksum's extent among them: MCEZ13 covers all but its last two bytes, every other model the
 * whole device.
 */
typedef struct {
	const char *name;
	uint16_t size;     /* nominal device size, bytes                                   */
	uint16_t cksum;    /* offset of the checksum byte                                  */
	uint16_t cksum_len;/* bytes covered by the sum; 0 = the whole device (K-2)         */
	uint16_t chan;     /* offset of the channel table                                  */
	uint16_t band;     /* offset of the band byte; index is bits 4-6                   */
	uint16_t refdiv;   /* offset of the reference divider pair (2 x 16-bit big-endian) */
	uint8_t nchan;     /* channel slots                                                */
	uint8_t stride;    /* bytes per channel record                                     */
	uint8_t tx;        /* offset of the TX triplet within a record                     */
	uint8_t rx;        /* offset of the RX triplet within a record                     */
	uint8_t numbered;  /* 1 = record carries a BCD number at +0 and a trakmode at +1   */
	const mc_flag *flags;
	uint8_t nflags;
	/* PL / CTCSS (K-14).  All zero on models where it is not implemented or not established. */
	uint16_t pl_tone;  /* the single-tone slot                                         */
	uint16_t pl_list;  /* base of the selectable tone list                             */
	uint16_t pl_count; /* byte whose HIGH nibble holds the number of selectable tones  */
	uint16_t pl_mode;  /* mode byte: 0x60 single, 0xE0 selectable; 0 = model has none  */
	uint16_t pl_dec;   /* PL DECODER table base (MCEZ13 only); 0 = model cannot decode */
	uint8_t pl_max;    /* entries in the list                                          */
	unsigned pl_k;     /* encoder scale x 10000 -- 79840 on EVA/EZ9, 79844 on MCEZ13   */
	const struct mc_timer *timers; /* K-16, NULL where the block is not measured        */
	uint8_t ntimers;
	uint16_t aak;      /* auto-acknowledge delay byte (K-15); 0 = not established here */
	const char *about; /* which radios this describes, for --list-models               */
} mc_model;

const mc_model *mc_model_by_name(const char *name);
const mc_model *mc_model_by_index(size_t i); /* NULL past the end; for enumeration */

/* Largest device this tool handles, so callers can size a buffer without allocating. */
#define MC_IMG_MAX 1024

/* Detection is ADVISORY: it reports what fits and proposes one, and the user may override.  Size
 * plus a valid checksum is all the evidence a file carries, and eva_56 and eva_sel5 are
 * indistinguishable by layout -- `note` says so when more than one fits.  Returns NULL if nothing
 * does. */
const mc_model *mc_model_detect(const uint8_t *bytes, size_t len, char *note, size_t notesz);

/* ---- image ---------------------------------------------------------------------------------- */
typedef struct {
	const mc_model *model;
	uint8_t *bytes;
	size_t len;
} mc_image;

/* Does this image actually contain everything the model addresses?
 *
 * Every accessor below indexes at fixed model offsets.  Nothing else validates that the buffer is
 * long enough, so a truncated file -- or the right file with the wrong --model -- would read past
 * the end.  Callers MUST run this before using an image they did not construct themselves.
 * Returns 0 if the image is usable, or the number of bytes the model needs when it is not.
 */
size_t mc_image_check(const mc_image *img);

/* Checksum (K-2).  The stored byte is chosen so the covered range sums to 0xFF mod 256.  That
 * range is the whole device on every model but MCEZ13, which covers all but its last two bytes --
 * so this is per-model data, not a constant. */
uint8_t mc_checksum_stored(const mc_image *img);
uint8_t mc_checksum_total(const mc_image *img);
int mc_checksum_valid(const mc_image *img);
/* Recompute the stored byte so the image becomes valid.  Returns the byte written. */
uint8_t mc_checksum_fix(mc_image *img);

/* Band (K-10).  Index 7 means unprogrammed: ask the user, do not treat it as an error. */
int mc_band_index(const mc_image *img);
int mc_band_raster(const mc_image *img);
/* Channel-spacing divisor P for a band index, or 0 if the band does not determine one. */
unsigned mc_band_p(int band_index);

uint16_t mc_refdiv(const mc_image *img, int which); /* which = 0 or 1 */

/* ---- frequency codec (K-10, K-11) ----------------------------------------------------------
 *   coarse = ((b0 & 3) << 8) | b1
 *   step   = (b0 & 4) ? 3125 : 2500
 *   hz     = (coarse * P + b2) * step
 * The RX field holds the local oscillator; the displayed RX frequency is this + MC_IF_HZ.
 */
#define MC_IF_HZ 21400000u

uint32_t mc_freq_decode(const uint8_t raw[3], unsigned p);
/* Canonical (K-11): always emits b2 < p.  `flags` supplies b0 bits 3-7, which carry per-channel
 * option bits and must be preserved (K-22).  Returns 0 on success, -1 if hz is not representable. */
int mc_freq_encode(uint32_t hz, unsigned p, unsigned step, uint8_t flags, uint8_t out[3]);
/* K-11: b2 is a full byte but p <= 254, so b2 >= p is a legal alternate spelling that the original
 * software does emit.  Decoders must accept it; encoders must not produce it. */
int mc_freq_is_canonical(const uint8_t raw[3], unsigned p);

/* ---- channels (K-21, K-23, K-24) ------------------------------------------------------------ */
typedef enum {
	MC_CH_PROGRAMMED = 0, /* a real channel                                          */
	MC_CH_EMPTY,          /* allocated but unprogrammed: valid number, zero frequency */
	MC_CH_STALE           /* past the terminator: content is leftover, preserve it    */
} mc_chan_state;

typedef struct {
	int slot; /* 1-based */
	mc_chan_state state;
	uint8_t num;      /* BCD number byte; 0 when the model has no numbers */
	uint8_t raw[16];  /* the record verbatim, `stride` bytes */
	uint8_t txraw[3], rxraw[3];
	uint32_t tx_hz, rx_hz; /* rx_hz already includes MC_IF_HZ */
	int canonical;         /* both triplets have b2 < p */
} mc_channel;

/* K-23: the table is terminated, not sparse.  Returns the number of live channels and, via
 * `terminator`, the 1-based slot holding the 0xFF terminator (0 if the table runs to the end). */
int mc_channel_count(const mc_image *img, int *terminator);
/* Decode one 0-based slot.  `p` may be 0 when the band is unprogrammed, in which case the
 * frequencies are left at 0 and only the raw bytes are filled in. */
int mc_channel_get(const mc_image *img, int slot0, unsigned p, mc_channel *out);

/* ---- editing (K-11, K-22, K-30) -------------------------------------------------------------
 * These change only the bytes the field owns.  None of them touches the checksum; call
 * mc_checksum_fix afterwards, so that saving is an explicit act.
 */
typedef enum { MC_TX = 0, MC_RX } mc_dir;

/* Channel raster, from the band byte's bit 7.  Feeding this to the encoder is what makes bit 2 of
 * b0 come out right; MCEZ13 band 2 is 2500 Hz where the EVA samples are 3125. */
unsigned mc_step_hz(const mc_image *img);

/* Set one half of a channel record.  For MC_RX, `hz` is the DISPLAYED frequency and the first IF
 * is subtracted here.  Returns 0, or -1 if the frequency is not representable -- which the caller
 * must surface rather than clamp (U-3). */
int mc_channel_set_freq(mc_image *img, int slot0, mc_dir dir, uint32_t hz);

const mc_flag *mc_flags(const mc_model *m, size_t *n);
const mc_flag *mc_flag_by_name(const mc_model *m, const char *name);
int mc_flag_get(const mc_image *img, int slot0, const mc_flag *f);
void mc_flag_set(mc_image *img, int slot0, const mc_flag *f, int on);

/* ---- PL / CTCSS (K-14) ----------------------------------------------------------------------
 * Tones are held as round(7.984 x f_Hz), big-endian -- the same law the signalling tones use.  The
 * encoding is lossy (88.5 Hz stores as 707, which decodes to 88.55), so decoding snaps to the
 * standard tone list the original software itself carries.  Frequencies are in TENTHS of a Hz
 * throughout, so no float ever touches a codeplug.
 */
#define MC_PL_MAX 10

/* MC_PL_TABLE is for models with no mode byte at all -- MCEZ13 simply carries its tables. */
/* ---- auto-acknowledge delay (K-15) ----------------------------------------------------------
 * One byte, a count of 1/64 s.  Only the repair build of the original software ever asks for it,
 * which is why it went unmapped for years; see ../doc/BUILD_VARIANTS.md.
 */
#define MC_AAK_MIN_MS 16   /* count 1   */
#define MC_AAK_MAX_MS 1984 /* count 127 */

int mc_aak_supported(const mc_model *m);
/* The raw count <-> milliseconds, exposed because they are the law and are tested directly. */
unsigned mc_aak_encode_ms(unsigned ms);
unsigned mc_aak_decode_count(unsigned count);
/* 0 when the model has no such field, or when the stored count is 0 -- which the original never
 * writes, and which therefore means "nothing here", not "0 ms". */
unsigned mc_aak_get_ms(const mc_image *img);
/* Returns 0, or -1 if `ms` is outside 16..1984 -- refused, never clamped (U-3).  Bit 7 of the byte
 * is left exactly as found: the original never sets it and its meaning is unknown (K-30). */
int mc_aak_set_ms(mc_image *img, unsigned ms);

/* ---- timers (K-16) ---------------------------------------------------------------------------
 * The original's timers sub-screen -- reached with `T' from any option page -- shows twelve fields,
 * and it does not compute them one at a time: it walks four parallel word arrays of twelve entries,
 * (offset, mask, A, B), through a single reader.  Both 512-byte families carry that table byte for
 * byte identical (`MCEV_56' 0xC9B3, `MCEV9' 0x8C5D, and so on).  Its own law is
 *
 *     v       := ((cp[off] and $7F) shl 8 or cp[off+1]) and mask
 *     display := Round(v * 10 / A) + B
 *
 * -- a real division ending in the Turbo Pascal runtime's Round entry, not an integer `div'.  That
 * distinction is worth a millisecond: for the synthesiser lock (A = 12) truncation would show 80
 * where the radio shows 81.  A is also the unit: six fields print seconds (A = 1000) and the rest
 * milliseconds, so B is in whatever unit its field prints.
 *
 * This table keeps everything in milliseconds -- what the codeplug actually holds -- so `ms' here
 * can be finer than the original's own screen can show.  Its seconds fields round 10 ms of rekey
 * delay down to `0 sec'; the byte still holds 10 ms and MCprog says so.
 *
 * Values that the law cannot represent are REFUSED, never rounded into range (U-3), and every bit
 * outside `mask' -- the enable, carrier-override, forced-reset and mode flags that share these
 * words -- is left exactly as found (K-30).
 */
typedef struct mc_timer {
	const char *name;
	uint16_t off;    /* first byte of the value; two-byte fields are big-endian off:off+1    */
	uint16_t mask;   /* the bits that are the value; the rest are flags and are preserved    */
	uint8_t  width;  /* 1 or 2                                                               */
	uint16_t den;    /* ms = round(v * 10 / den) + add_ms -- `A' from the original's table   */
	uint16_t add_ms; /* `B', converted to ms                                                 */
} mc_timer;

size_t mc_timer_count(const mc_model *m);
const mc_timer *mc_timer_at(const mc_model *m, size_t i);
/* Milliseconds.  The raw law is exposed for the tests; 0 is a legitimate value here, unlike K-15. */
unsigned mc_timer_decode(const mc_timer *t, unsigned raw);
unsigned mc_timer_get_ms(const mc_image *img, size_t i);
/* 0, or -1 if `ms` is not representable under that timer's law. */
int mc_timer_set_ms(mc_image *img, size_t i, unsigned ms);

#define MC_PL_K_EVA   79840u  /* 7.9840 -- every EVA and EZ9 build */
#define MC_PL_K_EZ13  79844u  /* 7.9844 -- every MCEZ13 build      */

typedef enum { MC_PL_OFF = 0, MC_PL_SINGLE, MC_PL_SELECTABLE, MC_PL_TABLE } mc_pl_mode;

/* The standard list, index 0 = 0 meaning no PL.  Returns tenths of a Hz. */
unsigned mc_pl_standard(size_t i);
size_t mc_pl_standard_count(void);

/* The tone scale is NOT the same in every build.  Every EVA and EZ9 chain file carries the Turbo
 * Pascal real 7.984; every MCEZ13 build carries 7.9844 instead, and no 7.984 at all.  Theory says
 * 150 x 2^16 / (4.9248 MHz / 4) = 7.984405, so MCEZ13 holds the more accurate figure.  Of the 39
 * EIA tones only 118.8 Hz tells them apart -- 948 against 949 -- and MCEZ13 was measured storing
 * 949.  mc_pl_encode keeps the EVA law for callers that have no model; the image-level accessors
 * use the model's own. */
uint16_t mc_pl_encode(unsigned dhz);                 /* EVA / EZ9 law, k = 7.9840 */
unsigned mc_pl_decode(uint16_t word);
uint16_t mc_pl_encode_k(unsigned dhz, unsigned k);   /* k = scale x 10000 */
unsigned mc_pl_decode_k(uint16_t word, unsigned k);

int mc_pl_supported(const mc_model *m);
/* MCEZ13 is the only model that decodes PL as well as encoding it, and it uses a DIFFERENT law:
 * round(61.107 x f_Hz) rather than round(7.984 x f_Hz).  Measured at 67.0, 103.5 and 250.3 Hz,
 * which bounds the constant to [61.1063, 61.1087]. */
int mc_pl_has_decoder(const mc_model *m);
uint16_t mc_pl_dec_encode(unsigned dhz);
unsigned mc_pl_dec_decode(uint16_t word);
unsigned mc_pl_dec_get(const mc_image *img, int i);
int mc_pl_dec_set(mc_image *img, int i, unsigned dhz);
mc_pl_mode mc_pl_get_mode(const mc_image *img);
void mc_pl_set_mode(mc_image *img, mc_pl_mode mode);
/* Number of selectable tones in force; 0 when PL is off or the model has none. */
int mc_pl_get_count(const mc_image *img);
void mc_pl_set_count(mc_image *img, int n);
/* The single tone (MC_PL_SINGLE) or list entry `i` (MC_PL_SELECTABLE), in tenths of a Hz. */
unsigned mc_pl_get_tone(const mc_image *img, int i);
int mc_pl_set_tone(mc_image *img, int i, unsigned dhz);

/* ---- conformance dump ----------------------------------------------------------------------
 * Emits the exact format of testdata/codeplug/[*].vec.  Shared by the CLI and the test
 * suite so that what the tests check is what the tool prints.
 */
void mc_dump_vec(FILE *f, const mc_image *img, const char *path);

/* ---- software parity (P-2), used by the protocol layer -------------------------------------- */
uint8_t mc_parity_tx(uint8_t b);
/* Returns 0 and stores the 7-bit value on success, -1 on a parity error. */
int mc_parity_rx(uint8_t b, uint8_t *out);

#endif /* MC_CODEPLUG_H */
