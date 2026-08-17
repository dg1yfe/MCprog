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
/* MC micro serial protocol -- see ../../spec.md, sections 1-5 (P-n requirements).
 *
 * Layering: this file speaks in *logical* bytes, all <= 0x7F.  The 7O1-over-8N1 software parity of
 * P-2 belongs to the physical link and is applied inside a concrete transport, because it is a
 * property of the wire and not of the protocol.  The captures were logged post-mask, so a replay
 * transport passes bytes through unchanged while the serial transport adds and checks parity.
 */
#ifndef MC_PROTOCOL_H
#define MC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define MC_BLOCK 64        /* bytes per read/write record (P-23, P-25) */
#define MC_T_BYTE 190      /* ms per received byte (P-30) */
#define MC_T_BURN 800      /* ms for the write's second ACK (P-31) */
#define MC_IDENT_MAX 128

/* ---- transport ------------------------------------------------------------------------------ */
typedef struct mc_transport mc_transport;
struct mc_transport {
	/* Send n bytes.  Returns 0, or -1 with *err set. */
	int (*send)(mc_transport *t, const uint8_t *buf, size_t n);
	/* Receive exactly n bytes, waiting at most timeout_ms for each.  Returns the count received
	 * (< n on timeout), or -1 on a hard error. */
	int (*recv)(mc_transport *t, uint8_t *buf, size_t n, unsigned timeout_ms);
	/* Milliseconds on this transport's clock; replay returns the capture's own timeline. */
	unsigned (*now_ms)(mc_transport *t);
	char err[160];
};

/* ---- session -------------------------------------------------------------------------------- */
typedef struct {
	mc_transport *t;
	int pending_ack;       /* a read block is waiting to be acknowledged (see mc_read_block) */
	unsigned last_burn_ms; /* gap between the two write ACKs, for P-25 diagnostics */
	int last_nak_header;   /* P-24: 1 = the end-of-memory NAK arrived behind an echoed header */
	unsigned t_rx;         /* clock reading when the last byte arrived */
	char err[200];
	/* Optional wire log, for the U-4 protocol page.  `tx` is 1 for PC->radio. */
	void (*log)(void *ctx, int tx, const uint8_t *buf, size_t n);
	void *logctx;
} mc_session;

void mc_session_init(mc_session *s, mc_transport *t);

/* ---- nibble codec, P-1 ---------------------------------------------------------------------- */
/* Each byte becomes two characters, high nibble first, each 0x30 + nibble. */
void mc_nib_encode(const uint8_t *in, size_t n, uint8_t *out);
/* Returns 0, or -1 if any character is outside 0x30-0x3F.  `n` is the count of input characters
 * and must be even. */
int mc_nib_decode(const uint8_t *in, size_t n, uint8_t *out);

/* P-3: build a command header -- 3 command characters then 4 nibble-characters of address, high
 * nibble first.  Public so the framing can be pinned directly: both captures use only 64-byte
 * aligned addresses below 0x1000, leaving two of the four nibbles constant. */
void mc_put_header(uint8_t out[7], const char *cmd, uint16_t addr);

/* ---- commands ------------------------------------------------------------------------------- */
/* P-21: returns eeprom[addr], a single byte.  This is NOT the ident. */
int mc_probe(mc_session *s, uint16_t addr, uint8_t *val);
/* P-20: `*`.  Stores the ident, 0x1A-terminated, and its length. */
int mc_identify(mc_session *s, char *ident, size_t max, size_t *len);
/* The opening exchange both read captures perform: probe, identify, probe. */
int mc_connect(mc_session *s, char *ident, size_t max, size_t *len);

/* P-23/P-24: read one 64-byte record.  Returns 1 on data, 0 on the end of memory (either NAK
 * form), -1 on error.
 *
 * `chain` selects the acknowledgement behaviour, which the original varies by context and which a
 * byte-faithful implementation must reproduce: during a sequential read every record is
 * acknowledged, the ACK riding in front of the *next* command; a standalone verification read
 * inside a write session is not acknowledged at all.  Pass 1 when walking memory, 0 when verifying
 * a just-written record. [C, both captures]
 */
int mc_read_block(mc_session *s, uint16_t addr, uint8_t out[MC_BLOCK], int chain);

/* P-25: write one 64-byte record and wait for BOTH ACKs.  Never returns before the second. */
int mc_write_block(mc_session *s, uint16_t addr, const uint8_t in[MC_BLOCK]);

/* ---- sessions ------------------------------------------------------------------------------- */
/* P-41: walk from 0 until the radio NAKs.  Stores the image and its length; the caller checks the
 * checksum -- a real radio has been captured returning an invalid one, and the data is still
 * wanted. */
int mc_read_all(mc_session *s, uint8_t *buf, size_t max, size_t *len);

/* P-42: write every record, then read it back and compare.  `verify` is called per record with the
 * written and read-back bytes so the caller can apply the K-11 decoded-frequency comparison; pass
 * NULL for a byte-exact comparison.  Returns 0, or -1 with s->err naming record and offset. */
int mc_write_all(mc_session *s, const uint8_t *buf, size_t len,
                 int (*verify)(void *ctx, uint16_t addr, const uint8_t *want, const uint8_t *got),
                 void *ctx);

/* ---- replay transport ----------------------------------------------------------------------- */
/* Backed by a testdata/traces/[*].trace file.  Every byte sent must match the capture's
 * next PC->radio byte, so a divergence in what we transmit is a test failure at the exact byte.
 * That is what makes the P-25 ordering testable: sending anything between the two ACKs lands on an
 * RX event and fails.
 */
typedef struct mc_replay mc_replay;
mc_replay *mc_replay_open(const char *path, char *err, size_t errsz);
mc_transport *mc_replay_transport(mc_replay *r);
/* 0 if the whole capture was consumed; otherwise the index of the first unconsumed event. */
int mc_replay_exhausted(mc_replay *r);
const char *mc_replay_name(mc_replay *r);
void mc_replay_close(mc_replay *r);

#endif /* MC_PROTOCOL_H */
