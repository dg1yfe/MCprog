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
/* The radio side of the protocol, for loopback testing -- spec.md sections 3 and 5.
 *
 * Written from the spec rather than by mirroring the client, so that a shared misreading does not
 * cancel itself out.  It answers the four commands the captures show and both NAK forms.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mc/serial.h"

static void nap_ms(unsigned ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

static int addr_of(const uint8_t *chars, uint16_t *addr)
{
	int i;
	uint16_t a = 0;
	for (i = 0; i < 4; i++) {
		if (chars[i] < 0x30 || chars[i] > 0x3F)
			return -1;
		a = (uint16_t)((a << 4) | (chars[i] - 0x30));
	}
	*addr = a;
	return 0;
}

int mc_fake_serve(mc_fake *f, mc_transport *t, unsigned idle_ms)
{
	uint8_t b, cmd[6], payload[MC_BLOCK * 2], out[7 + MC_BLOCK * 2];
	int handled = 0;

	for (;;) {
		uint16_t addr;
		if (t->recv(t, &b, 1, idle_ms) != 1)
			return handled; /* idle: the session is over */

		if (b == 0x06 || b == 0x15)
			continue; /* the PC acknowledging a record (P-26); nothing to do */

		if (b == 0x2A) { /* P-20 identify */
			uint8_t enc[MC_IDENT_MAX * 2];
			mc_nib_encode((const uint8_t *)f->ident, f->identlen, enc);
			if (t->send(t, enc, f->identlen * 2) != 0)
				return handled;
			handled++;
			continue;
		}

		if (b != 0x28 && b != 0x29)
			continue; /* P-4: anything else is not a command */

		if (t->recv(t, cmd, 6, idle_ms) != 6)
			return handled;
		if (addr_of(cmd + 2, &addr) != 0)
			continue;

		if (b == 0x29 && cmd[0] == '0' && cmd[1] == '1') { /* P-21: one byte */
			memcpy(out, "(01", 3);
			memcpy(out + 3, cmd + 2, 4);
			mc_nib_encode(f->eep + ((size_t)addr < f->len ? addr : 0), 1, out + 7);
			if (t->send(t, out, 9) != 0)
				return handled;
		} else if (b == 0x29 && cmd[0] == '0' && cmd[1] == '2') { /* P-22: two bytes */
			memcpy(out, "(02", 3);
			memcpy(out + 3, cmd + 2, 4);
			mc_nib_encode(f->eep + ((size_t)addr + 1 < f->len ? addr : 0), 2, out + 7);
			if (t->send(t, out, 11) != 0)
				return handled;
		} else if (b == 0x29 && cmd[0] == '4' && cmd[1] == '0') { /* P-23 read */
			if ((size_t)addr + MC_BLOCK <= f->len) {
				memcpy(out, "(40", 3);
				memcpy(out + 3, cmd + 2, 4);
				mc_nib_encode(f->eep + addr, MC_BLOCK, out + 7);
				if (t->send(t, out, sizeof out) != 0)
					return handled;
			} else if (f->nak_header) {
				/* P-24: past the end, echo the header and then NAK. */
				uint8_t nak = 0x15;
				memcpy(out, "(40", 3);
				memcpy(out + 3, cmd + 2, 4);
				if (t->send(t, out, 7) != 0 || t->send(t, &nak, 1) != 0)
					return handled;
			} else {
				uint8_t nak = 0x15;
				if (t->send(t, &nak, 1) != 0)
					return handled;
			}
		} else if (b == 0x28 && cmd[0] == '4' && cmd[1] == '0') { /* P-25 write */
			uint8_t ack = 0x06;
			if (t->recv(t, payload, sizeof payload, MC_T_BYTE) != (int)sizeof payload)
				return handled;
			if ((size_t)addr + MC_BLOCK <= f->len &&
			    mc_nib_decode(payload, sizeof payload, f->eep + addr) == 0)
				f->writes++;
			/* Two ACKs: command accepted, then the burn completed. */
			if (t->send(t, &ack, 1) != 0)
				return handled;
			nap_ms(f->burn_ms);
			if (t->send(t, &ack, 1) != 0)
				return handled;
		} else {
			continue;
		}
		handled++;
	}
}
