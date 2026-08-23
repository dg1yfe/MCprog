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
	/* 1 = the P-11/P-12 opening sequence, which takes 1.8 s.  Despite the RS-232 names, neither
	 * line carries modem control here: DTR supplies the level shifter and RTS drives the radio's
	 * HUB/PGM input, which is what selects programming mode.  Off for a pty, which has neither. */
	int line_setup;
} mc_serial_opts;

/* baud 1200, software parity on, line setup on. */
void mc_serial_defaults(mc_serial_opts *o);

mc_transport *mc_serial_open(const char *device, const mc_serial_opts *o, char *err, size_t errsz);
void mc_serial_close(mc_transport *t);

/* List candidate serial devices, most-likely first, for when the user has not named one.
 * Fills `out` with up to `max` NUL-terminated paths and returns how many. */
int mc_serial_enumerate(char out[][64], int max);

/* Drive DTR and RTS explicitly: 1 asserts, 0 de-asserts, -1 leaves the line alone.  Returns 0, or
 * -1 if the platform cannot do it -- a pseudo-terminal has no such lines.
 *
 * Neither line is modem control here.  DTR supplies the interface's level shifter and RTS drives
 * the radio's HUB/PGM input, which selects programming mode; this exists so the selftest can find
 * out which combination a real radio actually answers on (P-11). */
int mc_serial_set_lines(mc_transport *t, int dtr, int rts);

/* Re-arm the radio on an already-open port: the P-12 pulse again -- everything down for 500 ms,
 * RTS up, 1300 ms.  Takes 1.8 s.  Returns 0 if the pulse went out, -1 on a port with no control
 * lines (a pty) or a transport that is not a serial port.
 *
 * RTS reaches the radio CPU's #NMI input, and the rising edge issues the NMI that (re-)starts
 * programming mode.  So this is not line housekeeping -- it is a command to the radio, and it is
 * how the 1987 software gets away with never keeping a session: it pulses before every single
 * transaction (`ser_OpenLine').
 *
 * The intended use is P-24a: after the end-of-memory NAK the radio answers nothing, and until now
 * a power cycle was the only known way back.  UNTESTED ON HARDWARE -- and note the P-12 hardware
 * note, where a radio went permanently deaf after RTS was de-asserted for several seconds across
 * two probe combinations.  This pulse is 500 ms and never asserts DTR, which is what the original
 * does; whether the distinction matters is exactly what needs measuring. */
int mc_serial_rearm(mc_transport *t);

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
