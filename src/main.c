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
/* Argument handling and mode dispatch.
 *
 * One binary: the radio and the file are two sources of the same thing, so asking for either leads
 * to the same editor.  Naming an output file makes it non-interactive instead, which is what a
 * script or a bug report wants.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/codeplug.h"
#include "mc/serial.h"
#include "mc/tui.h"

static uint8_t img_bytes[MC_IMG_MAX];
static FILE *tracef;
static int logseq;

static void usage(FILE *f)
{
	const mc_model *m;
	size_t i;
	fprintf(f,
	        "usage: mcprog [options] [file.DAT]\n"
	        "\n"
	        "  mcprog file.DAT                  edit a codeplug file\n"
	        "  mcprog --port DEV                read the radio, then edit it\n"
	        "  mcprog --port DEV --read f.DAT   read the radio to a file and exit\n"
	        "  mcprog --port DEV --identify     print what the radio says it is\n"
	        "  mcprog --dump-vec file.DAT       print the conformance decode of a file\n"
	        "\n"
	        "options:\n"
	        "  --model NAME      override model detection\n"
	        "  --log FILE        record the wire in .trace format (with --port)\n"
	        "  --baud N          default 1200; 0 leaves the port's speed alone\n"
	        "  --no-modem-init   skip the DTR/RTS opening sequence (1.8 s)\n"
	        "\n"
	        "Writing to a radio is not implemented yet.\n"
	        "models:");
	for (i = 0; (m = mc_model_by_index(i)) != NULL; i++)
		fprintf(f, " %s", m->name);
	fprintf(f, "\n");
}

static void wirelog(void *ctx, int tx, const uint8_t *buf, size_t n)
{
	mc_session *s = ctx;
	unsigned t = s->t->now_ms(s->t);
	size_t i;
	fprintf(tracef, "%s %-3d %-6u %-6u ", tx ? "TX" : "RX", logseq++, t, t);
	for (i = 0; i < n; i++)
		fprintf(tracef, "%02x", buf[i]);
	fputc('\n', tracef);
	fflush(tracef);
}

static long load_file(const char *file)
{
	FILE *f = fopen(file, "rb");
	long n;
	if (!f) {
		perror(file);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > MC_IMG_MAX) {
		fprintf(stderr, "mcprog: %s is %ld bytes, which is not a codeplug\n", file, n);
		fclose(f);
		return -1;
	}
	if (fread(img_bytes, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "mcprog: %s: short read\n", file);
		fclose(f);
		return -1;
	}
	fclose(f);
	return n;
}

static int save_raw(const char *file, size_t len)
{
	FILE *f = fopen(file, "wb");
	if (!f || fwrite(img_bytes, 1, len, f) != len) {
		fprintf(stderr, "mcprog: cannot write %s\n", file);
		if (f)
			fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

/* Read the radio into img_bytes.  Returns the length, or -1.  With `ident_only` it stops after the
 * ident, so asking what a radio is does not read all of it. */
static long read_radio(const char *port, const mc_serial_opts *o, const char *logpath,
                       int ident_only)
{
	mc_transport *t;
	mc_session s;
	char err[160], ident[MC_IDENT_MAX];
	size_t ilen = 0, len = 0;

	t = mc_serial_open(port, o, err, sizeof err);
	if (!t) {
		fprintf(stderr, "mcprog: %s\n", err);
		return -1;
	}
	mc_session_init(&s, t);
	if (logpath) {
		tracef = fopen(logpath, "w");
		if (!tracef) {
			perror(logpath);
			mc_serial_close(t);
			return -1;
		}
		fprintf(tracef, "# captured by mcprog from %s\n", port);
		fprintf(tracef, "# <dir> <seq> <t_first_ms> <t_last_ms> <hex>\n");
		fprintf(tracef, "TRACE mcprog\n");
		s.log = wirelog;
		s.logctx = &s;
	}
	if (mc_connect(&s, ident, sizeof ident, &ilen) != 0) {
		fprintf(stderr, "mcprog: %s\n", s.err);
		mc_serial_close(t);
		return -1;
	}
	printf("ident: %.*s\n", (int)(ilen ? ilen - 1 : 0), ident); /* without the 0x1A terminator */
	if (ident_only) {
		if (tracef)
			fclose(tracef);
		mc_serial_close(t);
		return 0;
	}
	if (mc_read_all(&s, img_bytes, sizeof img_bytes, &len) != 0) {
		fprintf(stderr, "mcprog: %s\n", s.err);
		mc_serial_close(t);
		return -1;
	}
	printf("read %u bytes (%u records)\n", (unsigned)len, (unsigned)(len / MC_BLOCK));
	if (tracef)
		fclose(tracef);
	mc_serial_close(t);
	return (long)len;
}

int main(int argc, char **argv)
{
	const char *port = NULL, *readto = NULL, *want = NULL, *logpath = NULL, *file = NULL;
	int identify = 0, dumpvec = 0, i;
	mc_serial_opts o;
	const mc_model *model = NULL;
	mc_image img;
	char note[160] = "";
	long len;

	mc_serial_defaults(&o);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc)
			port = argv[++i];
		else if (!strcmp(argv[i], "--read") && i + 1 < argc)
			readto = argv[++i];
		else if (!strcmp(argv[i], "--model") && i + 1 < argc)
			want = argv[++i];
		else if (!strcmp(argv[i], "--log") && i + 1 < argc)
			logpath = argv[++i];
		else if (!strcmp(argv[i], "--baud") && i + 1 < argc)
			o.baud = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-modem-init"))
			o.modem_init = 0;
		else if (!strcmp(argv[i], "--identify"))
			identify = 1;
		else if (!strcmp(argv[i], "--dump-vec"))
			dumpvec = 1;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "mcprog: unknown option %s\n", argv[i]);
			usage(stderr);
			return 2;
		} else
			file = argv[i];
	}
	if (!port && !file) {
		usage(stderr);
		return 2;
	}
	if (port && file) {
		fprintf(stderr, "mcprog: give a port or a file, not both\n");
		return 2;
	}

	if (port) {
		int ident_only = identify && !readto;
		len = read_radio(port, &o, logpath, ident_only);
		if (len < 0)
			return 1;
		if (ident_only)
			return 0; /* the ident is already printed */
	} else {
		len = load_file(file);
		if (len < 0)
			return 1;
	}

	if (want) {
		model = mc_model_by_name(want);
		if (!model) {
			fprintf(stderr, "mcprog: unknown model %s\n", want);
			return 2;
		}
		snprintf(note, sizeof note, "model %s given on the command line", want);
	} else {
		model = mc_model_detect(img_bytes, (size_t)len, note, sizeof note);
	}

	/* A read is worth keeping even when nothing recognises it -- P-41 says report and hand the
	 * data over, and a radio holding an unreadable codeplug is the one that most needs reading. */
	if (readto) {
		if (save_raw(readto, (size_t)len) != 0)
			return 1;
		printf("%s\n", note);
		if (model) {
			img.model = model;
			img.bytes = img_bytes;
			img.len = (size_t)len;
			printf("checksum %s\n", mc_checksum_valid(&img) ? "valid" : "INVALID (saved anyway)");
		}
		printf("wrote %s\n", readto);
		return 0;
	}
	if (!model) {
		fprintf(stderr, "mcprog: %s\nuse --model to say which it is\n", note);
		return 1;
	}
	img.model = model;
	img.bytes = img_bytes;
	img.len = (size_t)len;

	if (dumpvec) {
		mc_dump_vec(stdout, &img, file ? file : "(radio)");
		return 0;
	}
	return mc_tui_run(&img, file, note);
}
