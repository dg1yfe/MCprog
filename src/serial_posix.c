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
	unsigned baud;         /* 0 = unknown, so the wire-time floor cannot be computed */
	unsigned wire_free_ms; /* earliest this transport's clock can show an empty shift register */
	struct timespec t0;
} serial;

/* 1 start + 8 data + 1 stop.  PARENB is cleared -- parity is carried in bit 7 by software (P-10)
 * -- so there is no ninth bit on the wire. */
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

static unsigned elapsed_ms(const struct timespec *t0)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)((now.tv_sec - t0->tv_sec) * 1000 +
	                  (now.tv_nsec - t0->tv_nsec) / 1000000);
}

/* Sleep the WHOLE interval, signals included.
 *
 * nanosleep returns EINTR with the remainder untouched in `rem' when a signal lands, and discarding
 * that -- passing NULL, as this did -- silently shortens the wait.  Every caller here is one where
 * short is wrong: the two P-12 line-settling delays, and the P-31d wire-time floor, where cutting
 * the wait is precisely the bug the floor exists to prevent.  A stray SIGCHLD or SIGWINCH must not
 * be able to reintroduce it. */
static void nap_ms(unsigned ms)
{
	struct timespec ts, rem;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &rem) != 0) {
		if (errno != EINTR)
			break;
		ts = rem;
	}
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
	/* Record when these bytes can physically be gone.  write() returned as soon as the kernel
	 * buffered them; the wire is busy for another n * 10 / baud seconds.  See sr_drain. */
	if (s->baud) {
		unsigned now = elapsed_ms(&s->t0);
		unsigned busy = (unsigned)((n * SR_BITS_PER_BYTE * 1000ul + s->baud - 1) / s->baud);
		busy += SR_WIRE_MARGIN_MS + busy * SR_WIRE_MARGIN_PCT / 100;
		if (s->wire_free_ms < now)
			s->wire_free_ms = now;
		s->wire_free_ms += busy;
	}
	return 0;
}

/* Wait until the bytes already handed to send() can physically be off the wire -- mc_transport.drain
 * (P-31d).  write() on a tty returns once the bytes are BUFFERED, so without this the caller's
 * timeout starts more than a second before a 135-byte frame reaches a 1200-baud radio. */
static int sr_drain(mc_transport *t)
{
	serial *s = (serial *)t;
	unsigned now = elapsed_ms(&s->t0);

	/* Arithmetic, deliberately, and NOT tcdrain.
	 *
	 * tcdrain is the obvious call and it was tried first.  Two things are wrong with it.  On a
	 * USB-serial adapter (FTDI, CH340, CP210x, PL2303) the kernel driver knows its own queue but
	 * not the adapter's internal FIFO, so tcdrain can return while the frame is still being
	 * shifted out beyond the USB link -- reintroducing the very error P-31d is about, invisibly.
	 * And on a pty it was measured BLOCKING for two seconds, long enough for the peer in
	 * tests/test_serial.c to hit its idle timeout and exit, so the ACK never came at all.
	 *
	 * The floor needs neither driver nor kernel to be honest: n bytes at b baud cannot leave in
	 * less than n * 10 / b seconds.  sr_send accumulates that per frame and clamps it forward to
	 * the current time, so a port that really is busy is tracked and one that is idle costs
	 * nothing.  It cannot block, cannot hang, and cannot be lied to. */
	/* Against the DEADLINE, not a single sleep: re-reading the clock each time absorbs both timer
	 * granularity and any sleep that still came back early.  The floor is only worth having if it
	 * is never undershot. */
	while (s->baud && now < s->wire_free_ms) {
		nap_ms(s->wire_free_ms - now);
		now = elapsed_ms(&s->t0);
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

/* speed_t is an opaque encoding, so it has to be mapped back to a number to cost a byte in
 * milliseconds.  Anything unrecognised returns 0, which disables the floor rather than guessing:
 * a wrong floor would be worse than none, since it would silently shorten the ACK window again. */
static unsigned speed_to_baud(speed_t sp)
{
	switch (sp) {
	case B300:    return 300;
	case B600:    return 600;
	case B1200:   return 1200;
	case B2400:   return 2400;
	case B4800:   return 4800;
	case B9600:   return 9600;
	case B19200:  return 19200;
	case B38400:  return 38400;
	case B57600:  return 57600;
	case B115200: return 115200;
	default:      return 0;
	}
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
	/* What speed to charge the wire-time floor at (P-31d).
	 *
	 * An explicitly requested baud wins: the caller is stating what the wire runs at.  Otherwise
	 * -- `--baud 0', which exists to leave the port alone -- ask the port, because leaving the
	 * speed alone must not silently mean leaving the floor off; that is how P-31d would return.
	 *
	 * A pty is the exception and gets no floor at all: it has no wire, delivers at memory speed,
	 * and its termios speed is a fiction.  Charging it wire time makes the client sleep past
	 * acknowledgements the peer already sent, which shows up as a burn gap (P-31a) of zero. */
	if (o->baud)
		s->baud = o->baud;
	else if (ptsname(s->fd) != NULL)
		s->baud = 0;
	else
		s->baud = speed_to_baud(cfgetospeed(&tio));
	tcflush(s->fd, TCIOFLUSH);

	/* P-27: NO pulse here.  The 1987 software has no persistent "open" -- it runs ser_OpenLine
	 * at the start of each OPERATION, twice, and mc_session_arm() reproduces that.  Pulsing here
	 * as well would make three where the original makes two, and the whole point of this path is
	 * to be identical.  The port configuration above still happens on open, as it must. */
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
	s->t.drain = sr_drain;
	s->t.recv = sr_recv;
	s->t.now_ms = sr_now;
	/* NULL when the user said --no-line-setup, so the AUTOMATIC arming in mc_session_arm() does
	 * nothing.  An EXPLICIT mc_serial_rearm() still works: the selftest sets line_setup = 0
	 * because it drives the lines itself, and P-24a needs to pulse them deliberately. */
	s->t.rearm = o->line_setup ? mc_serial_rearm : NULL;
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
