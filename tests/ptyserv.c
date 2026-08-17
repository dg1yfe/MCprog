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
/* A radio on a pty, for trying mcprog without hardware.
 *
 *     ./build/ptyserv fixtures/eva9_real.bin
 *     /dev/ttys004                     <- prints the device, then serves until killed
 *     ./build/mcprog --port /dev/ttys004 --no-modem-init --baud 0 --enable-write
 *
 * The image file is loaded at startup and written back out when the server exits, so a write can be
 * inspected afterwards.  --baud 0 and --no-modem-init are wanted because a pty has neither a line
 * speed nor modem control lines.
 *
 * This serves the same src/fakeradio.c the tests use -- a separate implementation of the radio side,
 * so a misreading of the protocol shared with the client cannot cancel itself out.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif
#include "mc/codeplug.h"
#include "mc/serial.h"

static const char DEFAULT_IDENT[] = "EV9.01.00.11 455M11-3     5/6 Tone radio\x1a";

static uint8_t eep[MC_IMG_MAX];
static size_t eeplen;
static const char *path;

static void save(void)
{
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(eep, 1, eeplen, f);
		fclose(f);
	}
}

static void bye(int sig)
{
	(void)sig;
	save();
	_exit(0);
}

int main(int argc, char **argv)
{
	int master, slave;
	char name[128], err[160];
	FILE *f;
	long n;
	mc_serial_opts o;
	mc_transport *t;
	mc_fake fk;
	unsigned idle = 3600000; /* effectively forever; the signal handler is how it stops */

	if (argc < 2) {
		fprintf(stderr, "usage: ptyserv <image.bin> [idle_ms]\n");
		return 2;
	}
	path = argv[1];
	if (argc > 2)
		idle = (unsigned)strtoul(argv[2], NULL, 10);
	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > MC_IMG_MAX || fread(eep, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "ptyserv: %s is not a codeplug image\n", path);
		fclose(f);
		return 1;
	}
	fclose(f);
	eeplen = (size_t)n;

	if (openpty(&master, &slave, name, NULL, NULL) != 0) {
		perror("openpty");
		return 1;
	}
	/* The radio sits on the master end and the client opens the slave by name.  The slave fd stays
	 * open here so the pty survives the client closing and reopening it -- which is exactly what
	 * mcprog does when a write follows a read. */
	printf("%s\n", name);
	fflush(stdout);

	signal(SIGTERM, bye);
	signal(SIGINT, bye);
	mc_serial_defaults(&o);
	o.baud = 0;
	o.line_setup = 0;
	t = mc_serial_attach(master, &o, err, sizeof err);
	if (!t) {
		fprintf(stderr, "ptyserv: %s\n", err);
		return 1;
	}
	memset(&fk, 0, sizeof fk);
	fk.eep = eep;
	fk.len = eeplen;
	fk.ident = DEFAULT_IDENT;
	fk.identlen = sizeof DEFAULT_IDENT - 1;
	fk.burn_ms = 20; /* P-25's shape; the real 710 ms would make interactive use tedious */
	mc_fake_serve(&fk, t, idle);
	save();
	return 0;
}
