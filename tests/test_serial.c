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
/* Serial transport over a pty loopback -- spec.md P-2, P-30, and the sessions of section 5.
 *
 *     ./test_serial [repo-root]
 *
 * A pty is not a radio, but it is a real file descriptor with real termios and a real other end, so
 * everything between the protocol layer and the kernel is exercised: framing, the software parity
 * in both directions, poll-based timeouts, and the double-ACK wait.  The radio side runs in a
 * forked child from a separate implementation (src/fakeradio.c), so a shared misreading of the
 * spec cannot cancel itself out.
 */
#include <fcntl.h>
#include <termios.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif
#include "mc/codeplug.h"
#include "mc/serial.h"

static const char *ROOT = ".";
static int pass, fail;
static const char IDENT[] = "EV9.01.00.11 455M11-3     5/6 Tone radio\x1a";
#define IDENTLEN 41
#define CHILD_DUMP "/tmp/mc_fake_eeprom.bin"

static void ok(int cond, const char *req, const char *what)
{
	if (cond)
		pass++;
	else {
		fail++;
		printf("FAIL  [%s] %s\n", req, what);
	}
}

static void failf(const char *req, const char *fmt, ...)
{
	va_list ap;
	fail++;
	printf("FAIL  [%s] ", req);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static uint8_t *slurp(const char *rel, size_t *len)
{
	char path[512];
	FILE *f;
	uint8_t *b;
	long n;
	snprintf(path, sizeof path, "%s/%s", ROOT, rel);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return b;
}

/* Fork a fake radio onto the slave end.  Returns the child pid, or -1. */
static pid_t spawn_radio(int master, int slave, const uint8_t *image, size_t len, int radio_parity,
                         int nak_header)
{
	int ready[2];
	pid_t pid;
	char go;

	/* The child's tcsetattr/tcflush would otherwise race the parent's first command and discard
	 * it, so the parent waits until the radio end is configured and listening. */
	if (pipe(ready) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(ready[0]);
		close(ready[1]);
		return -1;
	}
	if (pid == 0) {
		mc_serial_opts o;
		mc_transport *t;
		char err[160];
		mc_fake f;
		uint8_t *eep = malloc(len);
		FILE *out;

		close(master);
		close(ready[0]);
		memcpy(eep, image, len);
		mc_serial_defaults(&o);
		o.baud = 0;         /* a pty has no line speed; do not slow the test to 1200 baud */
		o.line_setup = 0;   /* and no modem control lines to sequence */
		o.sw_parity = radio_parity;
		t = mc_serial_attach(slave, &o, err, sizeof err);
		if (!t)
			_exit(2);
		memset(&f, 0, sizeof f);
		f.eep = eep;
		f.len = len;
		f.ident = IDENT;
		f.identlen = IDENTLEN;
		f.nak_header = nak_header;
		f.burn_ms = 60; /* P-25: the two ACKs must be genuinely apart */
		if (write(ready[1], "1", 1) != 1)
			_exit(3);
		close(ready[1]);
		mc_fake_serve(&f, t, 2000);
		out = fopen(CHILD_DUMP, "wb");
		if (out) {
			fwrite(eep, 1, len, out);
			fclose(out);
		}
		_exit(0);
	}
	close(slave);
	close(ready[1]);
	if (read(ready[0], &go, 1) != 1) {
		close(ready[0]);
		return -1;
	}
	close(ready[0]);
	return pid;
}

static mc_transport *client(int master, int parity)
{
	mc_serial_opts o;
	char err[160];
	mc_transport *t;
	mc_serial_defaults(&o);
	o.baud = 0;
	o.line_setup = 0;
	o.sw_parity = parity;
	t = mc_serial_attach(master, &o, err, sizeof err);
	if (!t)
		failf("P-10", "attach: %s", err);
	return t;
}

/* ---- the port configuration itself, P-10 ----------------------------------------------------
 * A pty is 8-bit clean whatever CSIZE says, so the loopback cannot notice a port opened CS7 -- on
 * real hardware that would strip bit 7 and destroy the parity scheme.  The settings are therefore
 * asserted directly, by reading back what mc_serial_attach left in the termios.
 */
static void test_port_config(void)
{
	int master, slave;
	mc_transport *t;
	struct termios tio;

	if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
		failf("P-10", "openpty failed");
		return;
	}
	t = client(master, 1);
	if (!t) {
		close(master);
		close(slave);
		return;
	}
	if (tcgetattr(master, &tio) != 0) {
		failf("P-10", "tcgetattr failed");
	} else {
		ok((tio.c_cflag & CSIZE) == CS8, "P-10", "eight data bits -- bit 7 carries our parity");
		ok(!(tio.c_cflag & PARENB), "P-2", "hardware parity is OFF; P-2 does it in software");
		ok(!(tio.c_cflag & CSTOPB), "P-10", "one stop bit");
		ok(!(tio.c_cflag & CRTSCTS), "P-10", "no hardware flow control");
		ok(!(tio.c_iflag & ISTRIP), "P-2", "ISTRIP off, or the parity bit would be stripped");
		ok(!(tio.c_iflag & (IXON | IXOFF)), "P-10", "no software flow control");
		ok(!(tio.c_iflag & (INPCK | PARMRK)), "P-2", "no kernel parity checking or marking");
		ok(!(tio.c_lflag & (ICANON | ECHO)), "P-10", "raw: no line discipline, no echo");
	}
	mc_serial_close(t);
	close(master);
	close(slave);
}

/* ---- P-31d: the ACK clock must not start before the frame has left ---------------------------
 *
 * This is the test that eight hardware sessions across four radios paid for.  Every one of them
 * reported "write: no first ACK" and every read in the same session worked, because send() returns
 * when the kernel has BUFFERED the frame: 135 bytes at 1200 baud is 1125 ms on the wire and
 * MC_T_ACK1 is 400, so the window shut while the radio was receiving byte 48 of 135.  No radio
 * could have answered inside it, which means those runs measured mcprog and told us nothing about
 * the radio.
 *
 * The pty cannot reproduce it -- a pty moves bytes at memory speed whatever baud is set -- so what
 * is asserted here is the CONTRACT that makes it impossible: drain() exists, and it does not return
 * before the wire could physically be free.  A transport that loses drain, or one whose drain
 * trusts a driver that lies (every USB-serial adapter), fails this. */
static void test_drain_floor(void)
{
	int master, slave;
	mc_transport *t;
	uint8_t frame[7 + MC_BLOCK * 2];
	unsigned t0, dt, want;

	/* What the wire physically costs: 1 start + 8 data + 1 stop per byte, parity being in bit 7. */
	want = (unsigned)(sizeof frame * 10 * 1000ul / 1200);
	ok(want > MC_T_ACK1, "P-31d",
	   "a write frame takes longer to send than MC_T_ACK1 allows for the ACK");

	if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
		failf("P-31d", "openpty failed");
		return;
	}
	{	/* Explicitly 1200, unlike client(), because the floor is derived from the port's real
		 * speed -- which is the whole point of deriving it there rather than from the option. */
		mc_serial_opts o;
		char err[160];
		mc_serial_defaults(&o);
		o.baud = 1200;
		o.line_setup = 0;
		t = mc_serial_attach(master, &o, err, sizeof err);
		if (!t) {
			failf("P-31d", "attach: %s", err);
			close(master);
			close(slave);
			return;
		}
	}
	ok(t->drain != NULL, "P-31d", "a serial transport provides drain()");
	memset(frame, 0x30, sizeof frame);
	t0 = t->now_ms(t);
	if (t->send(t, frame, sizeof frame) != 0) {
		failf("P-31d", "send failed: %s", t->err);
	} else if (!t->drain) {
		failf("P-31d", "no drain to test");
	} else {
		if (t->drain(t) != 0)
			failf("P-31d", "drain failed: %s", t->err);
		dt = t->now_ms(t) - t0;
		/* Allow a little slack for timer granularity, but nothing like a frame's worth. */
		ok(dt + 20 >= want, "P-31d",
		   "drain() waits out the frame it was handed, not the write() call");
		if (dt + 20 < want)
			failf("P-31d", "drain returned after %u ms; the frame needs %u ms", dt, want);
	}
	mc_serial_close(t);
	close(master);
	close(slave);
}

/* ---- a full read and write session over the pty --------------------------------------------- */

static void test_session(int nak_header)
{
	uint8_t *image;
	size_t len = 0;
	int master, slave;
	pid_t pid;
	mc_transport *t;
	mc_session s;
	char ident[MC_IDENT_MAX];
	size_t ilen = 0, rlen = 0;
	uint8_t got[1024];

	image = slurp("fixtures/eva9_real.bin", &len);
	if (!image) {
		failf("P-41", "cannot load fixtures/eva9_real.bin");
		return;
	}
	unlink(CHILD_DUMP);
	if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
		failf("P-10", "openpty failed");
		free(image);
		return;
	}
	pid = spawn_radio(master, slave, image, len, 1, nak_header);
	if (pid < 0) {
		failf("P-10", "fork failed");
		free(image);
		return;
	}
	t = client(master, 1);
	if (!t) {
		free(image);
		return;
	}
	mc_session_init(&s, t);

	if (mc_connect(&s, ident, sizeof ident, &ilen) != 0)
		failf("P-40", "connect over pty: %s", s.err);
	else {
		ok(ilen == IDENTLEN && memcmp(ident, IDENT, IDENTLEN) == 0, "P-20",
		   "the ident survives the wire, parity and all");
	}

	if (mc_read_all(&s, got, sizeof got, &rlen) != 0)
		failf("P-41", "read_all over pty: %s", s.err);
	else {
		ok(rlen == len, nak_header ? "P-24" : "P-24",
		   nak_header ? "size discovery stops at the header-NAK"
		              : "size discovery stops at the bare NAK");
		ok(rlen == len && memcmp(got, image, len) == 0, "P-41",
		   "every byte read back over the wire matches the image served");
	}

	/* Now write a modified image and let mc_write_all verify each record as it goes. */
	{
		uint8_t *mod = malloc(len);
		mc_image ci;
		memcpy(mod, image, len);
		ci.model = mc_model_by_name("eva_sel5");
		ci.bytes = mod;
		ci.len = len;
		if (mc_channel_set_freq(&ci, 0, MC_TX, 145000000u) != 0)
			failf("K-11", "could not set the test frequency");
		mc_checksum_fix(&ci);
		if (mc_write_all(&s, mod, len, NULL, NULL) != 0)
			failf("P-42", "write_all over pty: %s", s.err);
		else {
			ok(s.last_burn_ms >= 50, "P-25",
			   "the client waited for the second ACK, not just the first");
			/* Read it back in a fresh session to be sure it is the radio that changed. */
			if (mc_read_all(&s, got, sizeof got, &rlen) == 0)
				ok(rlen == len && memcmp(got, mod, len) == 0, "P-42",
				   "a re-read returns exactly what was written");
			else
				failf("P-42", "re-read failed: %s", s.err);
		}
		free(mod);
	}

	mc_serial_close(t);
	close(master);
	waitpid(pid, NULL, 0);

	/* And confirm against the child's own copy, not just our read-back. */
	{
		size_t dlen = 0;
		uint8_t *dump;
		FILE *f = fopen(CHILD_DUMP, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			dlen = (size_t)ftell(f);
			fseek(f, 0, SEEK_SET);
			dump = malloc(dlen);
			if (fread(dump, 1, dlen, f) != dlen)
				dlen = 0;
			fclose(f);
			ok(dlen == len && dump[0xE3] == 0x44 && dump[0xE4] == 0x00, "P-42",
			   "the radio's own memory holds the edited channel");
			free(dump);
		} else {
			failf("P-42", "the fake radio wrote no dump");
		}
	}
	free(image);
}

/* ---- P-2: prove the parity is real and not merely symmetric --------------------------------- */

static void test_parity_mismatch(void)
{
	uint8_t *image;
	size_t len = 0;
	int master, slave;
	pid_t pid;
	mc_transport *t;
	mc_session s;
	char ident[MC_IDENT_MAX];
	size_t ilen = 0;

	image = slurp("fixtures/eva9_real.bin", &len);
	if (!image)
		return;
	if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
		free(image);
		return;
	}
	/* The radio sends without parity; a client that genuinely checks it must object. */
	pid = spawn_radio(master, slave, image, len, 0, 1);
	if (pid < 0) {
		free(image);
		return;
	}
	t = client(master, 1);
	if (t) {
		mc_session_init(&s, t);
		ok(mc_connect(&s, ident, sizeof ident, &ilen) != 0, "P-2",
		   "a reply without parity is rejected, so the check is doing something");
		mc_serial_close(t);
	}
	close(master);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	free(image);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		ROOT = argv[1];
	test_port_config();
	test_drain_floor();
	test_session(1); /* P-24 header-NAK form */
	test_session(0); /* P-24 bare NAK form */
	test_parity_mismatch();
	printf("\n%d passed, %d FAILED\n", pass, fail);
	return fail ? 1 : 0;
}
