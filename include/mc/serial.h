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
/* Serial transports -- spec.md P-2, P-10, P-11, P-12, P-30.
 *
 * The port is opened 8N1 and the 7O1 parity of P-2 is done in software here, at the boundary where
 * it belongs: a 7O1 frame and an 8N1 frame are both ten bit times, so the line waveform is
 * identical and the radio cannot tell, while USB-serial bridges -- which vary in their handling of
 * 7-bit modes and not at all in 8N1 -- are taken out of the question.
 */
#ifndef MC_SERIAL_H
#define MC_SERIAL_H

#include "mc/protocol.h"

typedef struct {
	unsigned baud;  /* 1200 for a radio (P-10); 0 leaves the port's current speed alone */
	int sw_parity;  /* 1 = add/verify odd parity in bit 7 (P-2) */
	int modem_init; /* 1 = the P-11/P-12 opening sequence, which takes 1.8 s */
} mc_serial_opts;

/* baud 1200, software parity on, modem init on. */
void mc_serial_defaults(mc_serial_opts *o);

mc_transport *mc_serial_open(const char *device, const mc_serial_opts *o, char *err, size_t errsz);
void mc_serial_close(mc_transport *t);

#ifndef _WIN32
/* Wrap a file descriptor that is already open -- used to run the protocol over a pty pair, which
 * is how the transport is tested without a radio. */
mc_transport *mc_serial_attach(int fd, const mc_serial_opts *o, char *err, size_t errsz);
#endif

/* ---- fake radio -----------------------------------------------------------------------------
 * The radio side of the protocol, for loopback testing.  It is deliberately a separate
 * implementation from the client rather than a mirror of it, so that a shared misreading of the
 * spec does not cancel out.
 */
typedef struct {
	uint8_t *eep;
	size_t len;
	const char *ident;
	size_t identlen;
	int nak_header;   /* 1 = echo the header before the NAK (P-24); 0 = bare NAK */
	unsigned burn_ms; /* delay between the two write ACKs (P-25) */
	int writes;       /* count of records written, for assertions */
} mc_fake;

/* Serve until `idle_ms` passes with no command.  Returns the number of commands handled. */
int mc_fake_serve(mc_fake *f, mc_transport *t, unsigned idle_ms);

#endif /* MC_SERIAL_H */
