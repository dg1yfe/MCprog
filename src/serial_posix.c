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
/* POSIX serial transport -- termios.  See spec.md P-2, P-10, P-11, P-12, P-30.
 *
 * Self-excludes on Windows, as serial_win32.c does on everything else, so both can sit in
 * the source list on every platform.
 */
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "mc/codeplug.h" /* mc_parity_tx / mc_parity_rx */
#include "mc/serial.h"

typedef struct {
	mc_transport t; /* first member: a transport pointer casts back to this */
	int fd;
	int owned;
	int sw_parity;
	struct timespec t0;
} serial;

void mc_serial_defaults(mc_serial_opts *o)
{
	o->baud = 1200;
	o->sw_parity = 1;
	o->line_setup = 1;
}

static unsigned elapsed_ms(const struct timespec *t0)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)((now.tv_sec - t0->tv_sec) * 1000 +
	                  (now.tv_nsec - t0->tv_nsec) / 1000000);
}

static void nap_ms(unsigned ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/* P-12, and the one thing that puts a radio into programming mode: everything down for 500 ms, then
 * RTS up and 1300 ms to settle.  DTR stays DOWN throughout (P-11) -- it is the level shifter's
 * negative rail, not a modem signal, and most USB adapters would raise it on open.
 *
 * RTS reaches the radio CPU's #NMI input, so the RISING edge is the whole point: it issues an NMI
 * and the radio (re-)starts its programming routine.  That is why this is a pulse and not a level,
 * and why the 1987 RSS calls its equivalent (`ser_OpenLine') before EVERY transaction rather than
 * once on open -- see spec.md P-12 and P-24a.
 *
 * On a pty these ioctls are meaningless and fail; that is not an error there.  Returns 0 if the
 * pulse was actually delivered, -1 if this port has no control lines. */
static int pulse_rts(int fd)
{
	int bits = 0;

	if (ioctl(fd, TIOCMSET, &bits) != 0)
		return -1;
	nap_ms(500);
	bits = TIOCM_RTS;
	ioctl(fd, TIOCMSET, &bits);
	nap_ms(1300);
	return 0;
}

static unsigned sr_now(mc_transport *t)
{
	return elapsed_ms(&((serial *)t)->t0);
}

static int sr_send(mc_transport *t, const uint8_t *buf, size_t n)
{
	serial *s = (serial *)t;
	uint8_t tmp[512];
	size_t off = 0;

	while (off < n) {
		size_t chunk = n - off, i;
		ssize_t w;
		if (chunk > sizeof tmp)
			chunk = sizeof tmp;
		for (i = 0; i < chunk; i++)
			tmp[i] = s->sw_parity ? mc_parity_tx(buf[off + i]) : buf[off + i];
		w = write(s->fd, tmp, chunk);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			snprintf(t->err, sizeof t->err, "write: %s", strerror(errno));
			return -1;
		}
		off += (size_t)w;
	}
	return 0;
}

static int sr_recv(mc_transport *t, uint8_t *buf, size_t n, unsigned timeout_ms)
{
	serial *s = (serial *)t;
	size_t got = 0;

	while (got < n) {
		struct pollfd p;
		uint8_t b;
		ssize_t r;
		int pr;

		p.fd = s->fd;
		p.events = POLLIN;
		pr = poll(&p, 1, (int)timeout_ms);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			snprintf(t->err, sizeof t->err, "poll: %s", strerror(errno));
			return -1;
		}
		if (pr == 0)
			break; /* P-30: the caller decides whether a short read is fatal */
		r = read(s->fd, &b, 1);
		if (r == 0)
			break; /* the other end closed */
		if (r < 0) {
			if (errno == EINTR)
				continue;
			snprintf(t->err, sizeof t->err, "read: %s", strerror(errno));
			return -1;
		}
		if (s->sw_parity) {
			uint8_t v;
			if (mc_parity_rx(b, &v) != 0) {
				snprintf(t->err, sizeof t->err, "parity error on byte %02X", b);
				return -1;
			}
			buf[got++] = v;
		} else {
			buf[got++] = b & 0x7F; /* P-4: received bytes are masked before interpretation */
		}
	}
	return (int)got;
}

static int configure(serial *s, const mc_serial_opts *o, char *err, size_t errsz)
{
	struct termios tio;

	if (tcgetattr(s->fd, &tio) != 0) {
		snprintf(err, errsz, "tcgetattr: %s", strerror(errno));
		return -1;
	}
	cfmakeraw(&tio);
	/* P-10 / P-2: eight data bits, no hardware parity, one stop bit -- the parity is ours. */
	tio.c_cflag &= (tcflag_t) ~(PARENB | PARODD | CSTOPB | CSIZE | CRTSCTS);
	tio.c_cflag |= CS8 | CLOCAL | CREAD;
	tio.c_iflag &= (tcflag_t) ~(IXON | IXOFF | IXANY | ISTRIP | INPCK | PARMRK);
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	if (o->baud) {
		speed_t sp = o->baud == 1200 ? B1200 : o->baud == 9600 ? B9600 : B0;
		if (sp == B0) {
			snprintf(err, errsz, "unsupported baud %u", o->baud);
			return -1;
		}
		if (cfsetispeed(&tio, sp) != 0 || cfsetospeed(&tio, sp) != 0) {
			snprintf(err, errsz, "cfsetspeed: %s", strerror(errno));
			return -1;
		}
	}
	if (tcsetattr(s->fd, TCSANOW, &tio) != 0) {
		snprintf(err, errsz, "tcsetattr: %s", strerror(errno));
		return -1;
	}
	tcflush(s->fd, TCIOFLUSH);

	if (o->line_setup)
		pulse_rts(s->fd);
	return 0;
}

/* USB adapters first: a real radio is almost always on one, and /dev/ttyS* on a modern machine is
 * usually a motherboard port with nothing attached.  macOS needs cu.* rather than tty.*, which
 * block on carrier detect. */
static const char *const PREFIXES[] = {
	"cu.usbserial", "cu.usbmodem", "cu.SLAB", "cu.wchusb", "cu.PL2303",
	"ttyUSB", "ttyACM", "ttyS",
};

int mc_serial_enumerate(char out[][64], int max)
{
	DIR *d = opendir("/dev");
	struct dirent *e;
	size_t p;
	int n = 0;

	if (!d)
		return 0;
	for (p = 0; p < sizeof PREFIXES / sizeof PREFIXES[0] && n < max; p++) {
		rewinddir(d);
		while ((e = readdir(d)) != NULL && n < max) {
			if (strncmp(e->d_name, PREFIXES[p], strlen(PREFIXES[p])) != 0)
				continue;
			/* ttyS0-3 only: higher ones are almost never real */
			if (strncmp(e->d_name, "ttyS", 4) == 0 &&
			    (e->d_name[4] < '0' || e->d_name[4] > '3' || e->d_name[5] != 0))
				continue;
			snprintf(out[n], 64, "/dev/%s", e->d_name);
			n++;
		}
	}
	closedir(d);
	return n;
}

int mc_serial_set_lines(mc_transport *t, int dtr, int rts)
{
	serial *s = (serial *)t;
	int bits = 0;

	if (ioctl(s->fd, TIOCMGET, &bits) != 0)
		return -1; /* a pty: the ioctl is meaningless there, and that is not an error to report */
	if (dtr >= 0)
		bits = dtr ? (bits | TIOCM_DTR) : (bits & ~TIOCM_DTR);
	if (rts >= 0)
		bits = rts ? (bits | TIOCM_RTS) : (bits & ~TIOCM_RTS);
	return ioctl(s->fd, TIOCMSET, &bits) == 0 ? 0 : -1;
}

int mc_serial_rearm(mc_transport *t)
{
	serial *s = (serial *)t;

	if (!t || t->send != sr_send)
		return -1;
	if (pulse_rts(s->fd) != 0)
		return -1;
	tcflush(s->fd, TCIOFLUSH); /* the radio restarted; anything still queued predates it */
	return 0;
}

static mc_transport *wrap(int fd, int owned, const mc_serial_opts *o, char *err, size_t errsz)
{
	serial *s = calloc(1, sizeof *s);
	mc_serial_opts def;

	if (!o) {
		mc_serial_defaults(&def);
		o = &def;
	}
	s->t.send = sr_send;
	s->t.recv = sr_recv;
	s->t.now_ms = sr_now;
	s->fd = fd;
	s->owned = owned;
	s->sw_parity = o->sw_parity;
	clock_gettime(CLOCK_MONOTONIC, &s->t0);
	if (configure(s, o, err, errsz) != 0) {
		if (owned)
			close(fd);
		free(s);
		return NULL;
	}
	return &s->t;
}

mc_transport *mc_serial_attach(int fd, const mc_serial_opts *o, char *err, size_t errsz)
{
	return wrap(fd, 0, o, err, errsz);
}

mc_transport *mc_serial_open(const char *device, const mc_serial_opts *o, char *err, size_t errsz)
{
	int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		snprintf(err, errsz, "%s: %s", device, strerror(errno));
		return NULL;
	}
	/* Clear O_NONBLOCK now that the open has completed; reads are paced by poll(). */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
	return wrap(fd, 1, o, err, errsz);
}

void mc_serial_close(mc_transport *t)
{
	serial *s = (serial *)t;
	if (!s)
		return;
	if (s->owned)
		close(s->fd);
	free(s);
}
#endif /* !_WIN32 */
