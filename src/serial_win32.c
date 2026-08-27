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
/* Win32 serial transport -- spec.md P-2, P-10, P-11, P-12, P-30.
 *
 * The POSIX file is the reference; this mirrors it so that the only difference between platforms is
 * how the port is opened and timed.  Ports above COM9 need the \\.\ prefix, which is applied here
 * so the caller can just say "COM12".
 */
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "mc/codeplug.h" /* mc_parity_tx / mc_parity_rx */
#include "mc/serial.h"

typedef struct {
	mc_transport t; /* first member: a transport pointer casts back to this */
	HANDLE h;
	int sw_parity;
	unsigned baud;         /* 0 = no wire time chargeable */
	unsigned wire_free_ms; /* earliest this clock can show an empty shift register (P-31d) */
	DWORD t0;
} serial;

/* 1 start + 8 data + 1 stop; parity rides in bit 7 by software, so there is no ninth bit. */
#define SR_BITS_PER_BYTE 10
/* The floor is a LOWER bound, and overshooting it is FREE while undershooting is not.  A reply that
 * arrives during the wait is buffered by the kernel, so reading it late costs nothing; a wait that
 * ends early spends the difference out of MC_T_ACK1, which is the whole defect.  So the bound is
 * padded, asymmetrically and on purpose:
 *
 *   - a fixed part, for the transfer crossing the link before the adapter starts shifting at all
 *     (full-speed USB schedules on 1 ms boundaries, and there is a hub in the path here);
 *   - a proportional part, for baud generators whose divisor is not exact.  FT232 at 1200 is exact;
 *     not every bridge and not every rate is.
 */
#define SR_WIRE_MARGIN_MS  3
#define SR_WIRE_MARGIN_PCT 1


void mc_serial_defaults(mc_serial_opts *o)
{
	o->baud = 1200;
	o->sw_parity = 1;
	o->line_setup = 1;
}

/* P-12: everything down for 500 ms, then RTS up and 1300 ms to settle.  DTR stays DOWN (P-11).
 *
 * RTS reaches the radio CPU's #NMI input, so the RISING edge is the point: it issues an NMI and the
 * radio (re-)starts its programming routine.  Hence a pulse, not a level -- and hence the 1987 RSS
 * does this before EVERY transaction rather than once on open (spec.md P-12, P-24a). */
static void pulse_rts(HANDLE h)
{
	EscapeCommFunction(h, CLRDTR);
	EscapeCommFunction(h, CLRRTS);
	Sleep(500);
	EscapeCommFunction(h, SETRTS);
	Sleep(1300);
}

static unsigned sr_now(mc_transport *t)
{
	return (unsigned)(GetTickCount() - ((serial *)t)->t0);
}

static int sr_send(mc_transport *t, const uint8_t *buf, size_t n)
{
	serial *s = (serial *)t;
	uint8_t tmp[512];
	size_t off = 0;

	while (off < n) {
		size_t chunk = n - off, i;
		DWORD wrote = 0;
		if (chunk > sizeof tmp)
			chunk = sizeof tmp;
		for (i = 0; i < chunk; i++)
			tmp[i] = s->sw_parity ? mc_parity_tx(buf[off + i]) : buf[off + i];
		if (!WriteFile(s->h, tmp, (DWORD)chunk, &wrote, NULL) || wrote == 0) {
			snprintf(t->err, sizeof t->err, "WriteFile failed (%lu)", (unsigned long)GetLastError());
			return -1;
		}
		off += wrote;
	}
	/* P-31d: WriteFile returns once the driver has the bytes, so record when they can actually
	 * be gone.  See sr_drain. */
	if (s->baud) {
		unsigned now = sr_now(t);
		unsigned busy = (unsigned)((n * SR_BITS_PER_BYTE * 1000ul + s->baud - 1) / s->baud);
		busy += SR_WIRE_MARGIN_MS + busy * SR_WIRE_MARGIN_PCT / 100;
		if (s->wire_free_ms < now)
			s->wire_free_ms = now;
		s->wire_free_ms += busy;
	}
	return 0;
}

/* mc_transport.drain -- see P-31d.  WriteFile on a COM handle returns once the bytes are in the
 * driver's queue, exactly as write() does on a tty, so the same 1125 ms-frame / 400 ms-window
 * mismatch applies here.
 *
 * Waiting, not FlushFileBuffers, for the reason the POSIX side does not use tcdrain: there is no
 * portable way to ask whether the transmit register is empty, and the calls that look like they
 * answer that report on the driver's own queue instead.  Waiting out the frame from the moment it
 * was handed to the kernel needs nothing from the driver and is the same answer on every port.
 * n bytes at b baud cannot leave in under n * 10 / b seconds. */
static int sr_drain(mc_transport *t)
{
	serial *s = (serial *)t;
	unsigned now = sr_now(t);
	/* Against the deadline, as on POSIX, so timer granularity cannot undershoot the floor. */
	while (s->baud && now < s->wire_free_ms) {
		Sleep(s->wire_free_ms - now);
		now = sr_now(t);
	}
	return 0;
}

static int sr_recv(mc_transport *t, uint8_t *buf, size_t n, unsigned timeout_ms)
{
	serial *s = (serial *)t;
	COMMTIMEOUTS to;
	size_t got = 0;

	/* Per-byte deadline, matching poll() on the POSIX side (P-30). */
	memset(&to, 0, sizeof to);
	to.ReadIntervalTimeout = MAXDWORD;
	to.ReadTotalTimeoutMultiplier = MAXDWORD;
	to.ReadTotalTimeoutConstant = timeout_ms;
	if (!SetCommTimeouts(s->h, &to)) {
		snprintf(t->err, sizeof t->err, "SetCommTimeouts failed (%lu)",
		         (unsigned long)GetLastError());
		return -1;
	}
	while (got < n) {
		uint8_t b;
		DWORD nread = 0;
		if (!ReadFile(s->h, &b, 1, &nread, NULL)) {
			snprintf(t->err, sizeof t->err, "ReadFile failed (%lu)",
			         (unsigned long)GetLastError());
			return -1;
		}
		if (nread == 0)
			break; /* timed out; a short read is the caller's to judge */
		if (s->sw_parity) {
			uint8_t v;
			if (mc_parity_rx(b, &v) != 0) {
				snprintf(t->err, sizeof t->err, "parity error on byte %02X", b);
				return -1;
			}
			buf[got++] = v;
		} else {
			buf[got++] = (uint8_t)(b & 0x7F); /* P-4 */
		}
	}
	return (int)got;
}

mc_transport *mc_serial_open(const char *device, const mc_serial_opts *o, char *err, size_t errsz)
{
	char name[64];
	serial *s;
	DCB dcb;
	mc_serial_opts def;
	HANDLE h;

	if (!o) {
		mc_serial_defaults(&def);
		o = &def;
	}
	/* COM10 and above are only reachable through the device namespace. */
	if (_strnicmp(device, "\\\\.\\", 4) != 0 && _strnicmp(device, "COM", 3) == 0)
		snprintf(name, sizeof name, "\\\\.\\%s", device);
	else
		snprintf(name, sizeof name, "%s", device);

	h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		snprintf(err, errsz, "%s: cannot open (%lu)", name, (unsigned long)GetLastError());
		return NULL;
	}
	memset(&dcb, 0, sizeof dcb);
	dcb.DCBlength = sizeof dcb;
	if (!GetCommState(h, &dcb)) {
		snprintf(err, errsz, "GetCommState failed (%lu)", (unsigned long)GetLastError());
		CloseHandle(h);
		return NULL;
	}
	/* P-10 / P-2: 8N1 with no flow control; the parity is ours, in software. */
	dcb.BaudRate = o->baud ? o->baud : dcb.BaudRate;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fParity = FALSE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDsrSensitivity = FALSE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fNull = FALSE;
	dcb.fAbortOnError = FALSE;
	/* P-11: DTR down, RTS up.  DTR feeds the level shifter through a PNP transistor and RTS is the
	 * radio's HUB/PGM line; most USB bridges raise both on open, which is the wrong state for
	 * both.  Set here as well as in EscapeCommFunction below, because the DCB is what the driver
	 * reapplies on later state changes. */
	dcb.fDtrControl = DTR_CONTROL_DISABLE;
	dcb.fRtsControl = RTS_CONTROL_ENABLE;
	if (!SetCommState(h, &dcb)) {
		snprintf(err, errsz, "SetCommState failed (%lu)", (unsigned long)GetLastError());
		CloseHandle(h);
		return NULL;
	}
	PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

	if (o->line_setup)
		pulse_rts(h);
	s = calloc(1, sizeof *s);
	s->t.send = sr_send;
	s->t.drain = sr_drain;
	s->baud = o->baud; /* a COM handle is never a pty, so the requested speed is the wire speed */
	s->t.recv = sr_recv;
	s->t.now_ms = sr_now;
	s->h = h;
	s->sw_parity = o->sw_parity;
	s->t0 = GetTickCount();
	return &s->t;
}

void mc_serial_close(mc_transport *t)
{
	serial *s = (serial *)t;
	if (!s)
		return;
	CloseHandle(s->h);
	free(s);
}
int mc_serial_enumerate(char out[][64], int max)
{
	int n = 0, i;
	for (i = 1; i <= 8 && n < max; i++)
		n += snprintf(out[n], 64, "COM%d", i) > 0 ? 1 : 0;
	return n;
}

int mc_serial_set_lines(mc_transport *t, int dtr, int rts)
{
	serial *s = (serial *)t;
	int ok = 1;

	if (dtr >= 0)
		ok &= EscapeCommFunction(s->h, dtr ? SETDTR : CLRDTR) ? 1 : 0;
	if (rts >= 0)
		ok &= EscapeCommFunction(s->h, rts ? SETRTS : CLRRTS) ? 1 : 0;
	return ok ? 0 : -1;
}

int mc_serial_rearm(mc_transport *t)
{
	serial *s = (serial *)t;

	if (!t)
		return -1;
	pulse_rts(s->h);
	PurgeComm(s->h, PURGE_RXCLEAR | PURGE_TXCLEAR); /* the radio restarted; queued bytes predate it */
	return 0;
}
#endif /* _WIN32 */
