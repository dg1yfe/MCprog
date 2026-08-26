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
#include <time.h>
#include "mc/codeplug.h"
#include "mc/serial.h"
#include "mc/tui.h"
#include "mc/selftest.h"
#include "mc/write.h"

static uint8_t img_bytes[MC_IMG_MAX];
/* Kept from the read so detection can use it: a real EVA's ident names its signalling, which is
 * the only thing that separates the two 512-byte models. */
static char radio_ident[MC_IDENT_MAX];
static size_t radio_ident_len;
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
	        "  mcprog --list-models             describe every model this build knows\n"
	        "  mcprog --new MODEL [--band N] F  create F as a factory-default codeplug, no radio\n"
	        "  mcprog --list-defaults           the factory defaults this build can create\n"
	        "\n"
	        "  mcprog --selftest report.md      first contact with a real radio: finds the port\n"
	        "                                   itself, probes, measures, and writes a report\n"
	        "\n"
	        "  mcprog --port DEV --enable-write            read, edit, then 'w' writes it back\n"
	        "  mcprog --port DEV --write f.DAT --enable-write   write a file to the radio\n"
	        "\n"
	        "options:\n"
	        "  --model NAME      override model detection\n"
	        "  --log FILE        record the wire in .trace format (with --port)\n"
	        "  --backup FILE     name the pre-write backup; default is a timestamped file in the\n"
	        "                    working directory, never overwritten (W-2)\n"
	        "  --baud N          default 1200; 0 leaves the port's speed alone\n"
	        "  --no-line-setup   skip the DTR/RTS opening sequence (1.8 s).  DTR supplies the\n"
	        "                    interface; RTS is the radio's HUB/PGM line, which selects\n"
	        "                    programming mode -- so a real radio needs this left on\n"
	        "  --enable-write    permit writing; without it no write path exists (W-1)\n"
	        "\n"
	        "A write always backs the radio up first, refuses unless every changed byte is one\n"
	        "this program writes, and reads every record back to verify it.\n"
	        "models:");
	for (i = 0; (m = mc_model_by_index(i)) != NULL; i++)
		fprintf(f, " %s", m->name);
	fprintf(f, "   (--list-models describes them)\n");
}

/* Sizes and offsets are shown because they are how a user checks that a codeplug of unknown origin
 * is the model they think it is; PL is shown because it is the one capability that differs in what
 * the editor will offer. */
static void list_models(void)
{
	const mc_model *m;
	size_t i;

	printf("%-10s %5s %5s %8s  %-7s %s\n", "name", "bytes", "cksum", "channels", "PL", "radios");
	for (i = 0; (m = mc_model_by_index(i)) != NULL; i++) {
		char chans[24], pl[16];
		snprintf(chans, sizeof chans, "%u x %u", m->nchan, m->stride);
		if (m->pl_dec)
			snprintf(pl, sizeof pl, "enc+dec");
		else if (m->pl_tone)
			snprintf(pl, sizeof pl, "enc");
		else
			snprintf(pl, sizeof pl, "-");
		printf("%-10s %5u 0x%03X %8s  %-7s %s\n", m->name, m->size, m->cksum, chans, pl,
		       m->about ? m->about : "");
		if (m->storno || m->rss_ident) {
			printf("%-10s %5s %5s %8s  %-7s ", "", "", "", "", "");
			if (m->storno)
				printf("Storno: %s%s", m->storno, m->rss_ident ? "; " : "");
			if (m->rss_ident)
				printf("original software wants ident \"%s\"", m->rss_ident);
			printf("\n");
		}
	}
	printf("\nStorno sold these radios as the CQM 5500 series.  There is no separate Storno model:\n"
	       "its programmer is the Motorola one relocated, speaking the same protocol to the same\n"
	       "codeplug -- driven side by side the two put byte-identical traffic on the wire.  The\n"
	       "Storno names above are so an owner of a CQM 5500 can find the right model, and --model\n"
	       "accepts them.\n"
	       "\nThe ident column is what the ORIGINAL software demands, and mcprog does NOT enforce it.\n"
	       "It cannot: a real EVA answers \"EV9.01.00.11\" and the 1987 Standard build refuses it as\n"
	       "INVALID TYPE, while the Master and Repair builds of the same version read it happily.\n"
	       "That is a limit of that software, not of the radio.\n"
	       "\nDetection is by size, checksum and -- where the format has one -- a marker in the\n"
	       "bytes: the Radius M110 names its family at 0x07..0x09.  The two 512-byte EVA models\n"
	       "have no marker and no other difference, so they cannot be told apart from a file at\n"
	       "all; from a radio the ident settles it.  Use --model to choose.\n"
	       "\n--model SKIPS detection entirely.  Naming a model whose layout does not match the\n"
	       "bytes will read and write the wrong offsets, so use it to resolve an ambiguity the\n"
	       "tool reports -- not to force a file open that it refused.\n");
}

/* `--new': create a codeplug file without a radio, the way the repair build's INITIALIZE does --
 * from a genuine factory default, not from a blank buffer.  See mc_default in codeplug.h for why
 * that distinction is not cosmetic. */
static void list_defaults(FILE *f)
{
	const mc_default *d;
	size_t i;

	fprintf(f, "factory defaults this build carries:\n");
	for (i = 0; (d = mc_default_by_index(i)) != NULL; i++) {
		if (d->band)
			fprintf(f, "  --new %-9s --band %d   %4zu bytes  (%s)\n",
			        d->model, d->band, d->len, d->note);
		else
			fprintf(f, "  --new %-9s             %4zu bytes  (%s)\n",
			        d->model, d->len, d->note);
	}
	fprintf(f, "\nThese are captures of what the original software's INITIALIZE produced, not\n"
	           "synthesised images: a codeplug contains many bytes this project has never mapped,\n"
	           "and building one from only the understood fields would be wrong undetectably.\n"
	           "Where no capture exists -- the Radius M110s -- mcprog will not invent one.\n");
}

static int create_new(const char *model, int band, const char *path, int force)
{
	const mc_default *d = mc_default_find(model, band);
	const mc_model *m;
	uint8_t buf[MC_IMG_MAX];
	mc_image img;
	FILE *f;

	if (!d) {
		if (mc_model_by_name(model) || mc_model_by_storno(model))
			fprintf(stderr, "mcprog: no factory default has been captured for %s%s\n\n",
			        model, band ? " in that band" : "");
		else
			fprintf(stderr, "mcprog: unknown model %s\n\n", model);
		list_defaults(stderr);
		return 2;
	}
	m = mc_model_by_name(d->model);
	if (!m) {
		fprintf(stderr, "mcprog: internal: default names a model this build lacks\n");
		return 2;
	}
	/* Check what we are about to emit rather than trusting the table: a default that did not
	 * satisfy its own model's checksum would be a corrupted capture, and writing it silently
	 * would hand the user a file the radio will reject. */
	memset(&img, 0, sizeof img);
	img.model = m;
	img.len = d->len;
	img.bytes = buf;                    /* mc_image.bytes is a POINTER, not an array */
	if (d->len > sizeof buf) {
		fprintf(stderr, "mcprog: internal: default larger than MC_IMG_MAX\n");
		return 2;
	}
	memcpy(buf, d->bytes, d->len);
	if (d->len != m->size || !mc_checksum_valid(&img)) {
		fprintf(stderr, "mcprog: internal: the built-in default for %s is not a valid %s image\n",
		        d->model, m->name);
		return 2;
	}
	if (!force) {
		FILE *t = fopen(path, "rb");
		if (t) {
			fclose(t);
			fprintf(stderr, "mcprog: %s exists -- refusing to overwrite it (use --force)\n", path);
			return 2;
		}
	}
	if (!(f = fopen(path, "wb")) || fwrite(d->bytes, 1, d->len, f) != d->len) {
		if (f) fclose(f);
		fprintf(stderr, "mcprog: cannot write %s\n", path);
		return 2;
	}
	fclose(f);
	printf("wrote %s: %s, %zu bytes, checksum 0x%02X valid\n",
	       path, m->name, d->len, mc_checksum_stored(&img));
	printf("a factory default as the original INITIALIZE produced it -- %s\n", d->note);
	printf("edit it with:  mcprog %s\n", path);
	return 0;
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

/* A read that cannot be decoded is still a read: 1200 baud takes the better part of a minute, and
 * a radio whose codeplug is unrecognised is exactly the one worth keeping a copy of.  Never exit
 * having thrown one away. */
static void rescue(size_t len, const char *why)
{
	char name[64];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	FILE *f;

	strftime(name, sizeof name, "mcprog-rescue-%Y%m%d-%H%M%S.dat", tm);
	f = fopen(name, "wb");
	if (!f || fwrite(img_bytes, 1, len, f) != len) {
		fprintf(stderr, "mcprog: %s, and the rescue copy could not be written either\n", why);
		if (f)
			fclose(f);
		return;
	}
	fclose(f);
	fprintf(stderr, "mcprog: %s\n       the %u bytes read were saved to %s -- nothing was lost\n",
	        why, (unsigned)len, name);
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
	memcpy(radio_ident, ident, ilen > sizeof radio_ident ? sizeof radio_ident : ilen);
	radio_ident_len = ilen > sizeof radio_ident ? sizeof radio_ident : ilen;
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

struct wctx {
	const char *port;
	const mc_serial_opts *opts;
	const char *backup;        /* --backup; NULL lets mc_write_radio name it */
};

/* Opening the port again for the write, rather than holding it across an editing session, keeps
 * the DTR/RTS sequence exactly as it is on a fresh connection and leaves nothing to go stale. */
static int do_write(void *ctx, const mc_image *img, char *msg, size_t msgsz)
{
	struct wctx *w = ctx;
	mc_transport *t;
	mc_session s;
	mc_write_report rep;
	char err[160];

	t = mc_serial_open(w->port, w->opts, err, sizeof err);
	if (!t) {
		snprintf(msg, msgsz, "%s", err);
		return -1;
	}
	mc_session_init(&s, t);
	if (mc_write_radio(&s, img, w->backup, &rep) != 0) {
		snprintf(msg, msgsz, "NOT written: %s", rep.err);
		mc_serial_close(t);
		return -1;
	}
	mc_serial_close(t);
	snprintf(msg, msgsz, "written and verified: %d records, %d bytes changed, backup in %s",
	         rep.records, rep.changed, rep.backup);
	return 0;
}

int main(int argc, char **argv)
{
	const char *port = NULL, *readto = NULL, *want = NULL, *logpath = NULL, *file = NULL;
	const char *backup = NULL;
	const char *writefrom = NULL, *selftest = NULL;
	const char *newmodel = NULL;
	int identify = 0, dumpvec = 0, enable_write = 0, band = 0, force = 0, i;
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
		else if (!strcmp(argv[i], "--backup") && i + 1 < argc)
			backup = argv[++i];
		else if (!strcmp(argv[i], "--baud") && i + 1 < argc)
			o.baud = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-line-setup") || !strcmp(argv[i], "--no-modem-init"))
			o.line_setup = 0; /* the old spelling still works; there was never a modem */
		else if (!strcmp(argv[i], "--identify"))
			identify = 1;
		else if (!strcmp(argv[i], "--dump-vec"))
			dumpvec = 1;
		else if (!strcmp(argv[i], "--selftest") && i + 1 < argc)
			selftest = argv[++i];
		else if (!strcmp(argv[i], "--new") && i + 1 < argc)
			newmodel = argv[++i];
		else if (!strcmp(argv[i], "--band") && i + 1 < argc)
			band = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--force"))
			force = 1;
		else if (!strcmp(argv[i], "--list-models")) {
			list_models();
			return 0;
		}
		else if (!strcmp(argv[i], "--list-defaults")) {
			list_defaults(stdout);
			return 0;
		}
		else if (!strcmp(argv[i], "--enable-write"))
			enable_write = 1;
		else if (!strcmp(argv[i], "--write") && i + 1 < argc)
			writefrom = argv[++i];
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
	if (newmodel) {
		const mc_model *nm;
		if (!file) {
			fprintf(stderr, "mcprog: --new needs a file to create, e.g. "
			                "mcprog --new eza_sel5 --band 2 new.DAT\n");
			return 2;
		}
		if (port || writefrom || readto || selftest) {
			fprintf(stderr, "mcprog: --new creates a file and does not touch a radio\n");
			return 2;
		}
		/* Storno's badging resolves here too, so `--new "CQM5500 EZA 9, SELECT 5"' works. */
		nm = mc_model_by_name(newmodel);
		if (!nm)
			nm = mc_model_by_storno(newmodel);
		return create_new(nm ? nm->name : newmodel, band, file, force);
	}
	if (!port && !file) {
		usage(stderr);
		return 2;
	}
	if (selftest) {
		mc_selftest_opts so;
		char trace[300], plug[300];
		/* No --port: the selftest finds the radio itself. */
		snprintf(trace, sizeof trace, "%s.trace", selftest);
		snprintf(plug, sizeof plug, "%s.dat", selftest);
		memset(&so, 0, sizeof so);
		so.port = port;      /* NULL is fine -- the selftest will go looking */
		so.opts = &o;
		so.report_path = selftest;
		so.trace_path = logpath ? logpath : trace;
		so.codeplug_path = readto ? readto : plug;
		so.probe_lines = 1;
		/* The write probe rewrites the radio's own bytes, so nothing about the radio changes --
		 * but it is still a write, and W-1 says a write needs asking for. */
		so.write_back = enable_write;
		return mc_selftest(&so) == 0 ? 0 : 1;
	}
	if (writefrom && (!port || !enable_write)) {
		fprintf(stderr, "mcprog: --write needs --port and --enable-write\n");
		return 2;
	}
	if (port && file) {
		fprintf(stderr, "mcprog: give a port or a file, not both\n");
		return 2;
	}

	if (writefrom) {
		len = load_file(writefrom); /* the payload comes from the file, not the radio */
		if (len < 0)
			return 1;
	} else if (port) {
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
		/* Accept Storno's own badging too -- `--model "CQM5500 EZA 9, SELECT 5"', or any unique
		 * fragment of it.  Tried only after the real names, so nothing an existing script passes
		 * can change meaning. */
		if (!model)
			model = mc_model_by_storno(want);
		if (!model) {
			fprintf(stderr, "mcprog: unknown model %s -- these are the ones this build knows:\n\n",
			        want);
			list_models();
			return 2;
		}
		/* --model skips detection, which is the point of it -- but not so far as to let a model
		 * claim bytes that announce themselves as something else.  A codeplug carrying a family
		 * marker names its own format, and overriding that is never resolving an ambiguity; it is
		 * a mistake, and an expensive one.  Forcing `eza_sel5' onto a Radius M110 puts the write
		 * counter into live channel data and makes mc_checksum_fix() rewrite the serial number. */
		const mc_model *marked = mc_model_marked(img_bytes, (size_t)len);
		if (marked && marked != model) {
			fprintf(stderr,
			        "mcprog: refusing --model %s: these bytes carry the \"%s\" marker of model "
			        "%s\n", want, marked->tag, marked->name);
			fprintf(stderr,
			        "       that marker is part of the format, so the file is a %s codeplug and\n"
			        "       %s's offsets do not describe it. Use --model %s, or no --model at "
			        "all.\n", marked->name, want, marked->name);
			return 2;
		}
		snprintf(note, sizeof note, "model %s given on the command line", want);
	} else {
		model = mc_model_detect_ident(img_bytes, (size_t)len, radio_ident_len ? radio_ident : NULL,
		                              radio_ident_len, note, sizeof note);
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
			if (mc_image_check(&img))
				printf("model %s does not fit %u bytes; saved raw, not decoded\n",
				       model->name, (unsigned)len);
			else
				printf("checksum %s\n",
				       mc_checksum_valid(&img) ? "valid" : "INVALID (saved anyway)");
		}
		printf("wrote %s\n", readto);
		return 0;
	}
	if (!model) {
		/* A rescue copy is for bytes that exist nowhere else -- a radio read we could not decode.
		 * With --write the bytes came from a file the user already has. */
		if (port && !writefrom)
			rescue((size_t)len, note);
		else
			fprintf(stderr, "mcprog: %s\n", note);
		/* Naming a model does not help when the size matched and the checksum is what failed --
		 * that codeplug is damaged, and --model would only silence the diagnosis. */
		if (!strstr(note, "checksum"))
			fprintf(stderr, "       use --model to say which it is\n");
		return 1;
	}
	img.model = model;
	img.bytes = img_bytes;
	img.len = (size_t)len;
	{
		size_t need = mc_image_check(&img);
		if (need) {
			char why[160];
			snprintf(why, sizeof why,
			         "%s is %ld bytes but model %s addresses %u -- refusing to decode past the end",
			         file ? file : "the radio's reply", len, model->name, (unsigned)need);
			if (port)
				rescue((size_t)len, why);
			else
				fprintf(stderr, "mcprog: %s\n", why);
			return 1;
		}
	}

	if (dumpvec) {
		mc_dump_vec(stdout, &img, file ? file : "(radio)");
		return 0;
	}
	{
		struct wctx w;
		char msg[256];
		w.port = port;
		w.opts = &o;
		w.backup = backup;
		if (writefrom) { /* non-interactive: write the named file and report */
			int rc = do_write(&w, &img, msg, sizeof msg);
			printf("%s\n", msg);
			return rc ? 1 : 0;
		}
		return mc_tui_run(&img, file, note, (port && enable_write) ? do_write : NULL, &w);
	}
}
