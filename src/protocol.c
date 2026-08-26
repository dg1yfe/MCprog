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
/* MC micro serial protocol -- spec.md sections 1-5. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mc/protocol.h"

void mc_session_init(mc_session *s, mc_transport *t)
{
	memset(s, 0, sizeof *s);
	s->t = t;
}

static int fail(mc_session *s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->err, sizeof s->err, fmt, ap);
	va_end(ap);
	return -1;
}

/* ---- nibble codec, P-1 ---------------------------------------------------------------------- */

void mc_nib_encode(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t i;
	for (i = 0; i < n; i++) {
		out[i * 2] = (uint8_t)(0x30 + (in[i] >> 4));
		out[i * 2 + 1] = (uint8_t)(0x30 + (in[i] & 15));
	}
}

int mc_nib_decode(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t i;
	if (n % 2)
		return -1;
	for (i = 0; i < n; i += 2) {
		if (in[i] < 0x30 || in[i] > 0x3F || in[i + 1] < 0x30 || in[i + 1] > 0x3F)
			return -1;
		out[i / 2] = (uint8_t)(((in[i] - 0x30) << 4) | (in[i + 1] - 0x30));
	}
	return 0;
}

/* ---- primitives ----------------------------------------------------------------------------- */

static int tx(mc_session *s, const uint8_t *b, size_t n)
{
	if (s->log)
		s->log(s->logctx, 1, b, n);
	if (s->t->send(s->t, b, n) != 0)
		return fail(s, "send failed: %s", s->t->err);
	return 0;
}

static int rx(mc_session *s, uint8_t *b, size_t n, unsigned timeout)
{
	int got = s->t->recv(s->t, b, n, timeout);
	if (got > 0) {
		s->t_rx = s->t->now_ms(s->t);
		if (s->log)
			s->log(s->logctx, 0, b, (size_t)got);
	}
	if (got < 0)
		return fail(s, "receive failed: %s", s->t->err);
	return got;
}

/* A command is 3 command characters plus 4 nibble-characters of address (P-3). */
void mc_put_header(uint8_t p[7], const char *cmd, uint16_t addr)
{
	memcpy(p, cmd, 3);
	p[3] = (uint8_t)(0x30 + ((addr >> 12) & 15));
	p[4] = (uint8_t)(0x30 + ((addr >> 8) & 15));
	p[5] = (uint8_t)(0x30 + ((addr >> 4) & 15));
	p[6] = (uint8_t)(0x30 + (addr & 15));
}

static int hdr_matches(const uint8_t *p, const char *cmd, uint16_t addr)
{
	uint8_t want[7];
	mc_put_header(want, cmd, addr);
	return memcmp(p, want, 7) == 0;
}

int mc_probe(mc_session *s, uint16_t addr, uint8_t *val)
{
	uint8_t cmd[7], reply[9];
	int got;

	mc_put_header(cmd, ")01", addr);
	if (tx(s, cmd, 7) != 0)
		return -1;
	got = rx(s, reply, 9, MC_T_BYTE);
	if (got != 9)
		return fail(s, "probe at 0x%04X: wanted 9 bytes, got %d", addr, got);
	if (!hdr_matches(reply, "(01", addr))
		return fail(s, "probe at 0x%04X: bad reply header", addr);
	/* P-21: exactly one byte of codeplug data, not an ident. */
	if (mc_nib_decode(reply + 7, 2, val) != 0)
		return fail(s, "probe at 0x%04X: bad nibble encoding", addr);
	return 0;
}

/* P-27: open the line the way the 1987 software opens it, at the START OF AN OPERATION.
 *
 * MEASURED, not inferred from the prose.  Driving MCEZ9 under emulation with the radio's control
 * lines instrumented, a whole read session -- probe, identify, probe, four record reads, the
 * end-of-memory NAK -- produces exactly FOUR line transitions, and every one of them is at TX byte
 * ZERO:
 *
 *     dtr=0 rts=0   at TX byte 0        \  ser_OpenLine
 *     dtr=0 rts=1   at TX byte 0        /
 *     dtr=0 rts=0   at TX byte 0        \  and again
 *     dtr=0 rts=1   at TX byte 0        /
 *
 * A WRITE session (4 records, 575 bytes) gives the same four and no more.  So `ser_OpenLine' runs
 * TWICE, back to back, before the first byte of an operation -- and never again during it.  Not
 * before every command, not before every record.  spec.md P-12's "before every transaction" is
 * right but easy to misread as recurring mid-session; it does not.
 *
 * Why twice is not established [?].  Reproduced because the point is to be identical, and a second
 * NMI costs nothing but time.
 *
 * A transport with no control lines returns -1 from rearm and that is not an error -- there is
 * simply nothing to pulse.  Returns the number of pulses actually delivered.
 */
int mc_session_arm(mc_session *s)
{
	int n = 0, i;

	if (!s || !s->t || !s->t->rearm)
		return 0;
	for (i = 0; i < MC_ARM_PULSES; i++)
		if (s->t->rearm(s->t) == 0)
			n++;
	s->pending_ack = 0;    /* the radio restarted; no acknowledgement is owed across that */
	return n;
}

int mc_prewrite_read(mc_session *s, uint16_t addr, uint8_t out[MC_PREWRITE])
{
	uint8_t cmd[7], reply[7 + MC_PREWRITE * 2], val[MC_PREWRITE];
	int got, want = (int)sizeof reply;

	mc_put_header(cmd, ")0>", addr);
	if (tx(s, cmd, 7) != 0)
		return -1;
	got = rx(s, reply, (size_t)want, MC_T_BYTE);
	if (got != want)
		return fail(s, "pre-write read at 0x%04X: wanted %d bytes, got %d", addr, want, got);
	if (!hdr_matches(reply, "(0>", addr))
		return fail(s, "pre-write read at 0x%04X: bad reply header", addr);
	if (mc_nib_decode(reply + 7, MC_PREWRITE * 2, val) != 0)
		return fail(s, "pre-write read at 0x%04X: bad nibble encoding", addr);
	if (out)
		memcpy(out, val, MC_PREWRITE);
	return 0;
}

int mc_identify(mc_session *s, char *ident, size_t max, size_t *len)
{
	uint8_t star = 0x2A, b[2];
	size_t n = 0;

	if (tx(s, &star, 1) != 0)
		return -1;
	/* P-20: nibble-encoded, terminated by 0x1A.  Read pairs until the terminator appears. */
	for (;;) {
		uint8_t val;
		if (rx(s, b, 2, MC_T_BYTE) != 2)
			return fail(s, "identify: reply truncated after %u bytes", (unsigned)(n / 2));
		if (mc_nib_decode(b, 2, &val) != 0)
			return fail(s, "identify: bad nibble encoding at byte %u", (unsigned)(n / 2));
		if (n / 2 >= max)
			return fail(s, "identify: reply longer than %u bytes", (unsigned)max);
		ident[n / 2] = (char)val;
		n += 2;
		if (val == 0x1A)
			break;
	}
	if (len)
		*len = n / 2;
	return 0;
}

int mc_connect(mc_session *s, char *ident, size_t max, size_t *len)
{
	uint8_t v;
	/* Both read captures open exactly this way.  The ident is deliberately re-read rather than
	 * cached: the 2011 capture asks twice, 36 s apart, and both are answered (P-20). */
	if (mc_probe(s, 0, &v) != 0)
		return -1;
	if (mc_identify(s, ident, max, len) != 0)
		return -1;
	if (mc_probe(s, 0, &v) != 0)
		return -1;
	return 0;
}

int mc_read_block(mc_session *s, uint16_t addr, uint8_t out[MC_BLOCK], int chain)
{
	uint8_t cmd[8], reply[7 + MC_BLOCK * 2];
	size_t off = 0;
	int got;

	/* P-23a.  The radio masks the address to 10 bits -- the device byte carries only bits 9:8 --
	 * so a request past 0x03FF does not fail, it *aliases* back into bank 0 and returns plausible
	 * data for the wrong address.  The firmware's own header check rejects a high byte over 3, but
	 * an unaligned request can still walk off the top inside the record loop, which has no range
	 * check at all.  Refuse here rather than accept silently wrong bytes. */
	if (addr > MC_ADDR_MAX || addr + MC_BLOCK - 1 > MC_ADDR_MAX)
		return fail(s, "read 0x%04X: past the device's 0x%04X limit (would alias to bank 0)",
		            addr, MC_ADDR_MAX);

	/* The acknowledgement of the previous record rides in front of this command. */
	if (s->pending_ack)
		cmd[off++] = 0x06;
	mc_put_header(cmd + off, ")40", addr);
	off += 7;
	s->pending_ack = 0;
	if (tx(s, cmd, off) != 0)
		return -1;

	got = rx(s, reply, 7, MC_T_BYTE);
	if (got == 1 && reply[0] == 0x15) {
		s->last_nak_header = 0; /* P-24: the bare NAK form */
		return 0;
	}
	if (got != 7)
		return fail(s, "read 0x%04X: wanted a 7-byte header, got %d", addr, got);
	if (!hdr_matches(reply, "(40", addr))
		return fail(s, "read 0x%04X: bad reply header", addr);
	/* P-24: past the end the radio echoes the header and *then* NAKs. */
	got = rx(s, reply + 7, 1, MC_T_BYTE);
	if (got == 1 && reply[7] == 0x15)
		s->last_nak_header = 1;
	if (got == 1 && reply[7] == 0x15)
		return 0;
	if (got != 1)
		return fail(s, "read 0x%04X: no payload after the header", addr);
	got = rx(s, reply + 8, MC_BLOCK * 2 - 1, MC_T_BYTE);
	if (got != MC_BLOCK * 2 - 1)
		return fail(s, "read 0x%04X: payload truncated at %d of %d", addr, got + 1,
		            MC_BLOCK * 2);
	if (mc_nib_decode(reply + 7, MC_BLOCK * 2, out) != 0)
		return fail(s, "read 0x%04X: bad nibble encoding in payload", addr);
	if (chain)
		s->pending_ack = 1;
	return 1;
}

int mc_write_block(mc_session *s, uint16_t addr, const uint8_t in[MC_BLOCK])
{
	uint8_t frame[7 + MC_BLOCK * 2], a;
	unsigned t1, t2;

	mc_put_header(frame, "(40", addr);
	mc_nib_encode(in, MC_BLOCK, frame + 7);
	if (tx(s, frame, sizeof frame) != 0)
		return -1;

	/* P-25.  Two bare ACKs: the command was accepted, then the EEPROM burn finished.  Returning
	 * after the first and sending the next record desynchronises the radio immediately, so this
	 * function does not return until the second has arrived.
	 *
	 * P-31a: the first ACK is sent before any byte reaches the EEPROM, so it is prompt and gets a
	 * short timeout; only the second waits out the burn.  Separating them tells the two failures
	 * apart -- a radio that never took the record at all, versus one that took it and then stopped
	 * partway through committing it. */
	if (rx(s, &a, 1, MC_T_ACK1) != 1 || a != 0x06)
		return fail(s, "write 0x%04X: no first ACK (record not accepted; nothing was written)",
		            addr);
	t1 = s->t_rx;
	/* P-31b: there is no rollback.  The radio burns byte by byte and only ACKs at the end, so a
	 * failure here means an unknown prefix of this record is already committed. */
	if (rx(s, &a, 1, MC_T_BURN) != 1 || a != 0x06)
		return fail(s, "write 0x%04X: no second ACK (burn not confirmed; up to %d bytes of this "
		                "record may already be committed -- verify before retrying)",
		            addr, MC_BLOCK);
	t2 = s->t_rx;
	s->last_burn_ms = t2 - t1;
	return 0;
}

/* ---- sessions ------------------------------------------------------------------------------- */

int mc_read_all(mc_session *s, uint8_t *buf, size_t max, size_t *len)
{
	size_t off = 0;
	int r, records = 0;

	while (off + MC_BLOCK <= max) {
		r = mc_read_block(s, (uint16_t)off, buf + off, 1);
		if (r < 0)
			return -1;
		if (r == 0)
			break; /* end of memory */
		off += MC_BLOCK;
		if (++records > 64) /* P-41 caps the walk */
			return fail(s, "read: more than 64 records, refusing to continue");
	}
	if (len)
		*len = off;
	return 0;
}

int mc_write_all(mc_session *s, const uint8_t *buf, size_t len,
                 int (*verify)(void *ctx, uint16_t addr, const uint8_t *want, const uint8_t *got),
                 void *ctx)
{
	uint8_t back[MC_BLOCK];
	size_t off;

	if (len % MC_BLOCK)
		return fail(s, "write: %u bytes is not a whole number of records", (unsigned)len);
	for (off = 0; off < len; off += MC_BLOCK) {
		uint16_t addr = (uint16_t)off;
		if (mc_write_block(s, addr, buf + off) != 0)
			return -1;
		/* P-42: read the record back immediately.  This is a standalone read, so it takes no
		 * part in the acknowledgement chain (chain = 0). */
		if (mc_read_block(s, addr, back, 0) != 1)
			return fail(s, "write 0x%04X: read-back failed: %s", addr, s->err);
		if (verify) {
			if (verify(ctx, addr, buf + off, back) != 0)
				return fail(s, "write 0x%04X: verification rejected the read-back", addr);
		} else {
			size_t i;
			for (i = 0; i < MC_BLOCK; i++)
				if (back[i] != buf[off + i])
					return fail(s, "write 0x%04X: read-back differs at +%u: wrote %02X read "
					                "%02X",
					            addr, (unsigned)i, buf[off + i], back[i]);
		}
	}
	return 0;
}
