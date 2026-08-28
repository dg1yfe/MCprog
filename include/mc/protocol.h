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
#define MC_IDENT_MAX 128

/* The two write ACKs are not the same wait, and until the radio firmware was disassembled both
 * used one generous number.  P-31a: the radio sends the first ACK as soon as it has taken the
 * record into RAM -- before a single byte reaches the EEPROM -- so it arrives within a character
 * time.  Only the second waits out the burn.
 *
 * The burn is a *timed* loop, not a poll: eep_WriteByte delays LDX #$0BF7 / DEX / BNE = 3063 x 4
 * = 12252 cycles per byte, and at E = 4924800/4 Hz that is 9.95 ms.  With the bit-banged bus
 * transaction on top it is ~10.9 ms per byte, so a 64-byte record takes ~697 ms.  The old 800 ms
 * left 15 % margin over a number nobody had measured; it is raised here with the derivation
 * recorded, because a spurious timeout on the write path is expensive to diagnose and a generous
 * one costs only the time to notice a genuinely dead radio.  See spec.md P-31a. */
#define MC_T_ACK1 400      /* ms for the first ACK -- pre-burn, so it is prompt (P-31a) */
/* ms for the second ACK.  Was 2000, which is ~3x the 697 ms the EVA firmware's burn loop predicts
 * for 64 bytes (P-31a) and looked generous -- until 28 Aug 2026, when an M110 ACCEPTED a record for
 * the first time and then never confirmed the burn.  The wire log shows the first ACK arriving
 * 1140 ms after the frame was queued, exactly where P-31d says it should, and nothing in the
 * 2001 ms that followed.
 *
 * 697 ms is an EVA number, derived from EZA33.BIN's timed loop.  The M110 runs different firmware
 * (EZ3.01.00.44) whose burn constant nobody has read, so 2000 ms was never measured for it -- it
 * was an EVA figure with margin, applied to a radio it does not describe.  At 64 bytes, 2000 ms
 * allows only 31 ms per byte.
 *
 * A receive timeout costs nothing when the radio answers, and P-31 already argues that a spurious
 * timeout here is expensive to diagnose while a generous one costs only the wait on a genuinely
 * dead radio.  8000 ms allows 125 ms per byte, which is well past any plausible EEPROM. */
#define MC_T_BURN 8000

/* Microseconds of burn per byte, from the EVA firmware (spec.md P-31a).  Only that one radio's
 * constant is known, so this predicts rather than requires: the selftest compares it against the
 * measured gap and reports a divergence instead of failing on it. */
#define MC_BURN_US_PER_BYTE 10890
#define MC_BURN_EXPECT_MS(n) ((unsigned)(((n) * (unsigned long)MC_BURN_US_PER_BYTE) / 1000))

/* The radio's own limits, enforced by proto_ReadHeader in firmware: a count over 0x40 or an
 * address high byte over 0x03 is NAKed before anything else happens (spec.md P-23a). */
#define MC_ADDR_MAX 0x03FF
#define MC_COUNT_MAX 0x40

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
	/* OPTIONAL, may be NULL.  Re-run the line-opening sequence: everything down 500 ms, RTS up,
	 * 1300 ms.  RTS reaches the radio CPU's #NMI, so the rising edge restarts its programming
	 * routine -- which is how the 1987 software begins every operation (P-12, P-27).  NULL on any
	 * transport without control lines (replay, pty, fake radio), and callers must treat a NULL or
	 * a failure as "nothing to do" rather than as an error. */
	int (*rearm)(mc_transport *t);
	/* OPTIONAL, may be NULL.  Block until every byte already handed to send() has actually left
	 * the port.
	 *
	 * send() on a tty returns as soon as the kernel has BUFFERED the bytes, which at 1200 baud is
	 * about 1.1 seconds before a 135-byte write frame reaches the radio.  Any timeout started
	 * straight after send() is therefore measuring the local write() call, not the radio.  See
	 * P-31d: this is why every hardware write attempt through 23 Aug 2026 reported "no first ACK"
	 * -- the 400 ms window expired while the radio was still receiving byte 48 of 135.
	 *
	 * NULL on transports with no wire (replay, fake radio, pty), where send() is already
	 * synchronous; callers treat NULL as "nothing to wait for". */
	int (*drain)(mc_transport *t);
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

/* P-26: the PRE-WRITE HEADER READ, `)0>' -- 14 bytes at `addr'.
 *
 * The command grammar is P-1's: the two characters after the sigil are nibbles giving the byte
 * count, so `)0>' is 0x0E = 14 and `)40' is 64.  The Radius M110 software sends this immediately
 * before programming and NOWHERE ELSE, which is why no MC micro capture contains it -- it is the
 * M110's counterpart to the MC micro's `)02' serial check (doc/M110.md, the write-back oracle).
 *
 * WHY IT IS HERE.  Eight write attempts across four real M110s, in two batches, all ended the same
 * way: every read succeeds, and the radio goes PERMANENTLY silent the instant `(40' is sent --
 * ident included, and an RTS pulse does not revive it.  The frame is not at fault; it is
 * byte-identical to the record the radio itself had just returned.  The one step the original
 * performs and mcprog did not is this one.  Whether that is the cause is NOT established: it is
 * the strongest candidate, and this exists so the next batch can test it.
 *
 * Returns 0 on a well-formed reply, -1 otherwise.  `out' may be NULL if the caller only wants the
 * exchange to have happened.
 */
#define MC_PREWRITE 14
int mc_prewrite_read(mc_session *s, uint16_t addr, uint8_t out[MC_PREWRITE]);

/* P-27: the 1987 software calls `ser_OpenLine' TWICE, back to back, at the start of every
 * operation, and not again within it -- measured, see mc_session_arm().  Callers run this at an
 * operation boundary (a read, a write), NOT per command.  Returns the pulses delivered; 0 on a
 * transport without control lines, which is not an error. */
#define MC_ARM_PULSES 2
int mc_session_arm(mc_session *s);

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
void mc_replay_close(mc_replay *r);

#endif /* MC_PROTOCOL_H */
