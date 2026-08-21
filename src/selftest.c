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
/* First contact with a real radio -- M6.  TEMPORARY; see include/mc/selftest.h.
 *
 * Every probe records what it observed, not merely whether it liked it: the point of running this
 * against hardware is to find out where the spec is wrong, and a report that only says PASS/FAIL
 * cannot do that.  Raw bytes go in the report so a reader can draw their own conclusion.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mc/selftest.h"
#include "mc/protocol.h"
#include "mc/write.h"

#define MAXPROBES 32

/* R_INFO is for probes with nothing to compare against -- `)02` appears in neither capture, so its
 * reply cannot be "as documented".  Calling it a pass would be inventing a source. */
typedef enum { R_PASS = 0, R_DIFFERS, R_FAIL, R_SKIP, R_INFO } verdict;

typedef struct {
	const char *id;      /* the spec clause this bears on */
	const char *what;
	verdict v;
	char expected[160];
	char observed[400];
} probe;

typedef struct {
	probe p[MAXPROBES];
	int n;
	FILE *trace;
	int logseq;
	mc_session s;
	unsigned t0;
} run;

static run R;

static const char *VERDICT[] = { "as documented", "DIFFERS", "FAILED", "skipped", "recorded" };

static probe *note(const char *id, const char *what, verdict v, const char *expected,
                   const char *fmt, ...)
{
	probe *p;
	va_list ap;

	if (R.n >= MAXPROBES)
		return NULL;
	p = &R.p[R.n++];
	p->id = id;
	p->what = what;
	p->v = v;
	snprintf(p->expected, sizeof p->expected, "%s", expected ? expected : "");
	va_start(ap, fmt);
	vsnprintf(p->observed, sizeof p->observed, fmt, ap);
	va_end(ap);
	printf("  %-7s %-42s %s\n", p->id, p->what, VERDICT[p->v]);
	fflush(stdout);
	return p;
}

/* hex, for the report -- raw evidence beats a verdict */
static void hex(char *out, size_t outsz, const uint8_t *b, size_t n)
{
	size_t i, k = 0;
	out[0] = 0;
	for (i = 0; i < n && k + 3 < outsz; i++)
		k += (size_t)snprintf(out + k, outsz - k, "%02X ", b[i]);
	if (k)
		out[k - 1] = 0;
}

/* printable form of an ident, which is text but not guaranteed to be */
static void printable(char *out, size_t outsz, const uint8_t *b, size_t n)
{
	size_t i, k = 0;
	for (i = 0; i < n && k + 5 < outsz; i++) {
		if (b[i] >= 0x20 && b[i] < 0x7F)
			out[k++] = (char)b[i];
		else
			k += (size_t)snprintf(out + k, outsz - k, "<%02X>", b[i]);
	}
	out[k] = 0;
}

static void wirelog(void *ctx, int tx, const uint8_t *buf, size_t n)
{
	mc_session *s = ctx;
	unsigned t = s->t->now_ms(s->t);
	size_t i;
	if (!R.trace)
		return;
	fprintf(R.trace, "%s %-3d %-6u %-6u ", tx ? "TX" : "RX", R.logseq++, t, t);
	for (i = 0; i < n; i++)
		fprintf(R.trace, "%02x", buf[i]);
	fputc('\n', R.trace);
	fflush(R.trace);
}

/* The selftest should need nobody watching it.  When that is impossible -- no cable, or a radio
 * that has dropped out of programming mode and can only be revived by hand -- say so in a way
 * that cannot be missed in a wall of probe output. */
static void action_required(const char *what, const char *why)
{
	printf("\n");
	printf("  ============================================================\n");
	printf("   ACTION REQUIRED: %s\n", what);
	printf("   %s\n", why);
	printf("  ============================================================\n\n");
	fflush(stdout);
}

/* ---- 1. the control lines (P-11) -------------------------------------------------------------
 * The one experiment that cannot be done any other way, and the one most likely to be wrong.
 */
struct linecase {
	int dtr, rts;
	const char *name;
};
static const struct linecase LINES[] = {
	{ 0, 1, "DTR de-asserted, RTS asserted   (P-12, what MCprog does)" },
	{ 1, 1, "DTR asserted,    RTS asserted" },
	{ 0, 0, "DTR de-asserted, RTS de-asserted" },
	{ 1, 0, "DTR asserted,    RTS de-asserted" },
};

static void nap(mc_transport *t, unsigned ms)
{
	unsigned end = t->now_ms(t) + ms;
	uint8_t junk[16];
	while (t->now_ms(t) < end)
		t->recv(t, junk, sizeof junk, 50); /* drains as it waits, so stale bytes do not confuse */
}

/* Try one combination.  On success the port is left OPEN and returned through `keep`, because
 * closing it drops RTS -- see the note above.  Returns the ident length, 0 if the radio said
 * nothing, or -1 if this port has no control lines at all (a pseudo-terminal). */
static int try_lines(const mc_selftest_opts *o, const struct linecase *lc, char *ident,
                     size_t identsz, mc_transport **keep)
{
	mc_transport *t;
	mc_serial_opts so = *o->opts;
	mc_session s;
	char err[160];
	size_t len = 0;

	*keep = NULL;
	so.line_setup = 0; /* the selftest drives the lines itself */
	t = mc_serial_open(o->port, &so, err, sizeof err);
	if (!t)
		return 0;
	if (mc_serial_set_lines(t, lc->dtr, lc->rts) != 0) {
		mc_serial_close(t);
		return -1;
	}
	/* P-12's own timing, so a positive result means the documented sequence works */
	nap(t, 500);
	mc_serial_set_lines(t, lc->dtr, lc->rts);
	nap(t, 1300);
	mc_session_init(&s, t);
	if (mc_identify(&s, ident, identsz, &len) != 0)
		len = 0;
	if (len)
		*keep = t; /* hold it open; the rest of the session runs on this very port */
	else
		mc_serial_close(t);
	return (int)len;
}

/* Wait for a radio to start answering again, so a power cycle needs no keypress afterwards.
 * Returns 1 if it came back. */
static int wait_for_radio(const mc_selftest_opts *o, const struct linecase *lc, unsigned seconds)
{
	char ident[MC_IDENT_MAX];
	mc_transport *keep = NULL;
	unsigned i;

	for (i = 0; i < seconds; i++) {
		if (try_lines(o, lc, ident, sizeof ident, &keep) > 0) {
			if (keep)
				mc_serial_close(keep);
			printf("   radio is answering again -- carrying on\n");
			return 1;
		}
		printf("   waiting for the radio ... %us\r", seconds - i);
		fflush(stdout);
	}
	printf("\n");
	return 0;
}

/* Returns the index of the first combination the radio answered on, or -1 if none did (or if the
 * port has no control lines), and hands back that still-open port through `keep`.
 *
 * It STOPS at the first success, and that is not an optimisation.  The first hardware run showed
 * why: two combinations answered, the probe went on to try the two with RTS de-asserted, and from
 * that moment the radio never spoke again -- not to the remaining probe, not to the session that
 * followed, which re-asserted RTS and still got nothing.  Dropping RTS takes the radio out of
 * programming mode and it does not come back without a power cycle.  So: once it answers, stop
 * asking, and keep the port open, because closing it drops RTS too. */
static int probe_lines(const mc_selftest_opts *o, mc_transport **keep)
{
	char ident[MC_IDENT_MAX], obs[400] = "";
	size_t i, k = 0;
	int len, winner = -1, unsupported = 0;

	*keep = NULL;
	for (i = 0; i < sizeof LINES / sizeof LINES[0]; i++) {
		printf("      trying %s ... ", LINES[i].name);
		fflush(stdout);
		len = try_lines(o, &LINES[i], ident, sizeof ident, keep);
		if (len < 0) {
			unsupported++;
			printf("no control lines on this port\n");
			continue;
		}
		printf("%s\n", len ? "answered" : "silent");
		k += (size_t)snprintf(obs + k, sizeof obs - k, "%sDTR=%d RTS=%d: %s", k ? "; " : "",
		                      LINES[i].dtr, LINES[i].rts, len ? "answered" : "silent");
		if (len) {
			winner = (int)i;
			if (i + 1 < sizeof LINES / sizeof LINES[0])
				k += (size_t)snprintf(obs + k, sizeof obs - k,
				                      "; stopped there -- the remaining %u combination(s) were "
				                      "NOT tried, because de-asserting RTS takes the radio out "
				                      "of programming mode",
				                      (unsigned)(sizeof LINES / sizeof LINES[0] - i - 1));
			break;
		}
	}
	if (unsupported) {
		note("P-11", "control lines: DTR down, RTS up", R_SKIP,
		     "the radio answers with DTR de-asserted and RTS asserted",
		     "this port has no modem control lines -- it is a pseudo-terminal, not a serial port, "
		     "so the question cannot be asked here");
		return -1;
	}
	if (winner == 0)
		note("P-11", "control lines: DTR down, RTS up", R_PASS,
		     "the radio answers with DTR de-asserted and RTS asserted", "%s", obs);
	else if (winner > 0)
		note("P-11", "control lines: DTR down, RTS up", R_DIFFERS,
		     "the radio answers with DTR de-asserted and RTS asserted",
		     "MCprog's own combination was SILENT; this radio answered on DTR=%d RTS=%d. %s -- "
		     "P-11/P-12 are wrong and mcprog will not talk to this radio until they are fixed. "
		     "The rest of this report was gathered using the combination that worked",
		     LINES[winner].dtr, LINES[winner].rts, obs);
	else {
		/* The one thing no program can do for itself.  Ask once, plainly, then watch for the
		 * radio to come back so the run continues without anybody pressing a key. */
		action_required("power-cycle the radio",
		                "no line combination answered.  A radio that has left programming mode "
		                "only returns after its power is cycled; check the cable too if this "
		                "repeats.");
		if (wait_for_radio(o, &LINES[0], 60)) {
			char id2[MC_IDENT_MAX];
			if (try_lines(o, &LINES[0], id2, sizeof id2, keep) > 0) {
				note("P-11", "control lines: DTR down, RTS up", R_PASS,
				     "the radio answers with DTR de-asserted and RTS asserted",
				     "silent at first; answered on DTR=0 RTS=1 after a power cycle. %s", obs);
				return 0;
			}
		}
		note("P-11", "control lines: DTR down, RTS up", R_FAIL,
		     "the radio answers with DTR de-asserted and RTS asserted",
		     "no combination answered, and it did not return within 60 s of waiting. %s", obs);
	}
	return winner;
}

/* Find a radio without being told where to look.  Tries each candidate device with the line
 * combination MCprog uses; the first that answers wins.  This is the difference between "run
 * mcprog --selftest" and "work out what your serial port is called first". */
static int find_port(const mc_selftest_opts *o, char *out, size_t outsz)
{
	char devs[16][64], ident[MC_IDENT_MAX];
	int n, i;

	n = mc_serial_enumerate(devs, 16);
	if (n == 0) {
		action_required("connect the interface",
		                "no serial devices were found at all -- is the USB adapter plugged in?");
		return 0;
	}
	printf("  looking for a radio on %d port%s\n", n, n == 1 ? "" : "s");
	for (i = 0; i < n; i++) {
		mc_selftest_opts cand = *o;
		mc_transport *keep = NULL;
		int len;

		cand.port = devs[i];
		printf("      %-22s ... ", devs[i]);
		fflush(stdout);
		len = try_lines(&cand, &LINES[0], ident, sizeof ident, &keep);
		if (keep)
			mc_serial_close(keep);
		if (len > 0) {
			printf("a radio answered\n");
			snprintf(out, outsz, "%s", devs[i]);
			return 1;
		}
		printf("%s\n", len < 0 ? "no control lines" : "silent");
	}
	action_required("check the cable, and power-cycle the radio",
	                "every port was silent.  RTS must reach the radio's HUB/PGM line, and a radio "
	                "that has left programming mode needs a power cycle to return.");
	return 0;
}

/* ---- 2. ident (P-20) -------------------------------------------------------------------------- */

static void probe_ident(void)
{
	char a[MC_IDENT_MAX], b[MC_IDENT_MAX], pr[300];
	size_t la = 0, lb = 0;

	if (mc_identify(&R.s, a, sizeof a, &la) != 0) {
		note("P-20", "ident (`*`)", R_FAIL, "41 bytes, 0x1A-terminated",
		     "%s -- the radio answered the line probe and has since gone quiet", R.s.err);
		return;
	}
	printable(pr, sizeof pr, (const uint8_t *)a, la);
	note("P-20", "ident (`*`)", la == 41 ? R_PASS : R_DIFFERS,
	     "41 bytes ending 0x1A, e.g. `EV9.01.00.11 455M11-3     5/6 Tone radio`",
	     "%u bytes: `%s`", (unsigned)la, pr);

	/* the capture answers `*` twice, 36 s apart; if this radio answers only once, every later
	 * assumption about reconnecting is wrong */
	if (mc_identify(&R.s, b, sizeof b, &lb) != 0)
		note("P-20", "ident is answered more than once", R_DIFFERS,
		     "a second `*` is answered too", "the second `*` got nothing: %s", R.s.err);
	else
		note("P-20", "ident is answered more than once", la == lb && memcmp(a, b, la) == 0
		                                                     ? R_PASS : R_DIFFERS,
		     "a second `*` returns the same string",
		     "second reply is %u bytes and %s", (unsigned)lb,
		     (la == lb && memcmp(a, b, la) == 0) ? "identical" : "DIFFERENT");
}

/* ---- 3. the single-byte read, and the unknown `)02` ------------------------------------------- */

static void probe_single(void)
{
	uint8_t v0 = 0, v1 = 0;

	if (mc_probe(&R.s, 0x0000, &v0) != 0 || mc_probe(&R.s, 0x0001, &v1) != 0) {
		note("P-21", "`)01` single-byte read", R_FAIL, "returns eeprom[addr]", "%s", R.s.err);
		return;
	}
	note("P-21", "`)01` single-byte read", R_PASS, "returns eeprom[addr], one byte",
	     "0x0000 = %02X, 0x0001 = %02X (checked against the full read below)", v0, v1);
}

/* `)02` appears in neither capture, so there is no documented reply to compare against.  Send it
 * and record whatever comes back; two bytes is the standing guess. */
static void probe_02(void)
{
	uint8_t hdr[7], reply[32];
	char h[128];
	int got;

	mc_put_header(hdr, ")02", 0x0000);
	if (R.s.log)
		R.s.log(R.s.logctx, 1, hdr, 7); /* this probe bypasses the session, so log it by hand */
	if (R.s.t->send(R.s.t, hdr, 7) != 0) {
		note("P-22", "`)02` (never seen in any capture)", R_FAIL, "unknown -- this is the point",
		     "send failed: %s", R.s.t->err);
		return;
	}
	got = R.s.t->recv(R.s.t, reply, sizeof reply, MC_T_BYTE);
	if (got > 0 && R.s.log)
		R.s.log(R.s.logctx, 0, reply, (size_t)got);
	if (got <= 0) {
		note("P-22", "`)02` (never seen in any capture)", R_INFO,
		     "unknown; the guess is a 2-byte reply",
		     "no reply within %u ms -- the radio may not implement it", MC_T_BYTE);
		return;
	}
	hex(h, sizeof h, reply, (size_t)got);
	note("P-22", "`)02` (never seen in any capture)", R_INFO,
	     "unknown; the guess is `(02`+addr followed by 2 bytes", "%d bytes: %s", got, h);
	nap(R.s.t, 300); /* let anything trailing drain before the next probe */
}

/* ---- 4. the full read, its timing, and the end-of-memory form (P-24, P-41) --------------------- */

static size_t probe_read(uint8_t *img, size_t max, const mc_model **model, char *note_out,
                         size_t notesz)
{
	unsigned t_start = R.s.t->now_ms(R.s.t), dt;
	size_t len = 0;

	if (mc_read_all(&R.s, img, max, &len) != 0) {
		note("P-41", "read the whole EEPROM", R_FAIL, "records from 0 until the radio NAKs",
		     "%s (got %u bytes first)", R.s.err, (unsigned)len);
		return 0;
	}
	dt = R.s.t->now_ms(R.s.t) - t_start;
	note("P-41", "read the whole EEPROM", R_PASS, "records from 0 until the radio NAKs",
	     "%u bytes in %u records, %u ms (%u ms/record)", (unsigned)len,
	     (unsigned)(len / MC_BLOCK), dt, len ? dt / (unsigned)(len / MC_BLOCK) : 0);

	note("P-24", "end-of-memory NAK form", R_PASS,
	     "either a bare 0x15 or an echoed header then 0x15; both are accepted",
	     "this radio %s", R.s.last_nak_header ? "echoes the header, THEN NAKs"
	                                          : "sends a bare NAK");

	*model = mc_model_detect(img, len, note_out, notesz);
	return len;
}

static void probe_decode(const uint8_t *img, size_t len, const mc_model *model, const char *det)
{
	mc_image im;
	mc_channel c;
	int nch, term = 0;
	unsigned p;

	if (!model) {
		note("K-20", "identify the model from the bytes", R_DIFFERS,
		     "size and checksum match exactly one model", "%s", det);
		return;
	}
	im.model = model;
	im.bytes = (uint8_t *)img;
	im.len = len;
	note("K-20", "identify the model from the bytes", R_PASS,
	     "size and checksum match a known model", "%s", det);
	note("K-2", "checksum", mc_checksum_valid(&im) ? R_PASS : R_DIFFERS,
	     "the covered bytes sum to 0xFF",
	     "sums to 0x%02X%s", mc_checksum_total(&im),
	     mc_checksum_valid(&im) ? "" : " -- a real radio has been captured with an invalid one, so "
	                                   "this is worth reporting but is not necessarily a fault");

	p = mc_band_p(mc_band_index(&im));
	nch = mc_channel_count(&im, &term);
	if (mc_band_index(&im) == 7) {
		note("K-10", "band and channels", R_DIFFERS, "a programmed band, 1-4",
		     "band index 7: this radio is unprogrammed");
		return;
	}
	mc_channel_get(&im, 0, p, &c);
	note("K-10", "band and channels", R_PASS, "a programmed band and at least one channel",
	     "band %d (P=%u), %d channels%s; channel 1 TX %u.%05u RX %u.%05u MHz",
	     mc_band_index(&im), p, nch, term ? "" : " (table runs to the end)", c.tx_hz / 1000000u,
	     (c.tx_hz % 1000000u) / 10u, c.rx_hz / 1000000u, (c.rx_hz % 1000000u) / 10u);
}

/* ---- 5. the write path, on the radio's own bytes (P-25, P-42) --------------------------------- */

static void probe_write(const uint8_t *img, size_t len)
{
	uint8_t back[MC_BLOCK];
	unsigned t_start, dt;
	int rc;

	/* One record, and the one the radio already holds: nothing about the radio changes, but the
	 * whole write path runs -- framing, the double ACK, and the read-back. */
	t_start = R.s.t->now_ms(R.s.t);
	rc = mc_write_block(&R.s, 0x0000, img);
	dt = R.s.t->now_ms(R.s.t) - t_start;
	if (rc != 0) {
		note("P-25", "write one record (its own bytes, unchanged)", R_FAIL,
		     "two ACKs: one on acceptance, one when the burn finishes", "%s", R.s.err);
		return;
	}
	note("P-25", "write one record (its own bytes, unchanged)", R_PASS,
	     "two ACKs, the second roughly 710 ms after the first",
	     "accepted and burnt in %u ms; the gap between the two ACKs was %u ms", dt,
	     R.s.last_burn_ms);

	if (mc_read_block(&R.s, 0x0000, back, 0) != 1)
		note("P-42", "read the record back", R_FAIL, "the record reads back byte for byte",
		     "%s", R.s.err);
	else
		note("P-42", "read the record back", memcmp(back, img, MC_BLOCK) == 0 ? R_PASS : R_FAIL,
		     "the record reads back byte for byte",
		     memcmp(back, img, MC_BLOCK) == 0 ? "identical" : "DIFFERS from what was written");
	(void)len;
}

/* ---- the report -------------------------------------------------------------------------------
 * Markdown, because it is going into an issue or a document.  It leads with the answers, because
 * the person reading it wants to know whether the thing works.
 */
static void write_report(const mc_selftest_opts *o, const char *ident, size_t len,
                         const mc_model *model)
{
	FILE *f = fopen(o->report_path, "w");
	time_t now = time(NULL);
	char when[64];
	int i, differs = 0, failed = 0, skipped = 0, info = 0;

	if (!f) {
		fprintf(stderr, "mcprog: cannot write %s\n", o->report_path);
		return;
	}
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
	for (i = 0; i < R.n; i++) {
		differs += R.p[i].v == R_DIFFERS;
		failed += R.p[i].v == R_FAIL;
		skipped += R.p[i].v == R_SKIP;
		info += R.p[i].v == R_INFO;
	}

	fprintf(f, "# MCprog first-contact report\n\n");
	fprintf(f, "- **when** %s\n- **port** `%s`\n- **radio** `%s`\n", when, o->port,
	        ident[0] ? ident : "(not identified)");
	fprintf(f, "- **read** %u bytes, model %s\n", (unsigned)len, model ? model->name : "unknown");
	fprintf(f, "- **wire log** `%s` — every byte, timestamped, in the format the conformance "
	           "suite reads\n",
	        o->trace_path ? o->trace_path : "(none)");
	if (o->codeplug_path)
		fprintf(f, "- **codeplug** `%s`\n", o->codeplug_path);
	fprintf(f, "\n**%d probes: %d as documented, %d differ, %d failed", R.n,
	        R.n - differs - failed - skipped - info, differs, failed);
	if (skipped)
		fprintf(f, ", %d skipped", skipped);
	if (info)
		fprintf(f, ", %d recorded with nothing to compare against", info);
	fprintf(f, ".**\n\n");
	if (differs || failed)
		fprintf(f, "> Anything marked DIFFERS or FAILED is the interesting part: the spec was "
		           "written from captures and disassembly, and this is the first time it has met "
		           "a radio. Please send this file back with the wire log.\n\n");

	fprintf(f, "## Probes\n\n");
	for (i = 0; i < R.n; i++) {
		fprintf(f, "### %s — %s\n\n", R.p[i].id, R.p[i].what);
		fprintf(f, "- **verdict** %s\n", VERDICT[R.p[i].v]);
		if (R.p[i].expected[0])
			fprintf(f, "- **expected** %s\n", R.p[i].expected);
		fprintf(f, "- **observed** %s\n\n", R.p[i].observed);
	}

	fprintf(f, "## What is still not answered\n\n");
	fprintf(f, "These need a person, not a program:\n\n");
	fprintf(f, "- **Do the ten PL slots get indexed per channel?** No codeplug byte records a "
	           "channel-to-tone mapping, so if the radio does it, firmware decides. Program "
	           "several distinct tones through the original software, then key up on each channel "
	           "and listen.\n");
	fprintf(f, "- **The MCEZ13 two-byte write header.** The 1987 editor emits two leading bytes "
	           "its own reader does not expect. If the radio swallows them, a read-back after a "
	           "write by the original software will show it.\n");
	fprintf(f, "- **The write counter (W-5).** MCprog does not touch it, because its offset "
	           "differs per model and is only partly measured. Writing with the original software "
	           "and diffing the result would settle each model's offset.\n");

	fprintf(f, "\n---\n\nGenerated by `mcprog --selftest`. Radios are scarce, so please keep this "
	           "file and the wire log beside it: the first radio through this corrected four "
	           "clauses of the spec.\n");
	fclose(f);
	printf("\nreport: %s\n", o->report_path);
}

int mc_selftest(const mc_selftest_opts *o)
{
	static uint8_t img[MC_IMG_MAX];
	mc_transport *t = NULL;
	mc_serial_opts so;
	char err[160], ident[MC_IDENT_MAX], pr[300] = "", det[160] = "";
	const mc_model *model = NULL;
	size_t ilen = 0, len = 0;
	int winner = -1;
	mc_selftest_opts eff;
	char found[64];

	memset(&R, 0, sizeof R);
	if (!o->port) {
		/* `eff` and `found` are function-scoped deliberately: `o` points at them for the rest of
		 * the run, so neither may be block-local. */
		if (!find_port(o, found, sizeof found))
			return -1;
		eff = *o;
		eff.port = found;
		o = &eff;
	}
	printf("MCprog selftest on %s\n\n", o->port);

	if (o->probe_lines) {
		printf("  P-11: which control-line combination does this radio answer on?\n");
		winner = probe_lines(o, &t);
		if (winner >= 0 && t)
			printf("      -- keeping that port open for the rest of the run\n");
		printf("\n");
	}

	/* Reuse the port the probe left open.  Opening a fresh one would close this one first, and
	 * closing drops RTS -- which is what silenced the radio on the first hardware run. */
	if (!t) {
		so = *o->opts;
		t = mc_serial_open(o->port, &so, err, sizeof err);
		if (!t) {
			fprintf(stderr, "mcprog: %s\n", err);
			return -1;
		}
	}
	mc_session_init(&R.s, t);
	if (o->trace_path) {
		R.trace = fopen(o->trace_path, "w");
		if (R.trace) {
			fprintf(R.trace, "# mcprog selftest capture from %s\n", o->port);
			fprintf(R.trace, "# <dir> <seq> <t_first_ms> <t_last_ms> <hex>\n");
			fprintf(R.trace, "TRACE mcprog-selftest\n");
			R.s.log = wirelog;
			R.s.logctx = &R.s;
		}
	}

	ident[0] = 0;
	probe_ident();
	if (mc_identify(&R.s, ident, sizeof ident, &ilen) == 0)
		printable(pr, sizeof pr, (const uint8_t *)ident, ilen ? ilen - 1 : 0);
	probe_single();
	probe_02();
	len = probe_read(img, sizeof img, &model, det, sizeof det);
	if (len)
		probe_decode(img, len, model, det);

	if (len && o->codeplug_path) {
		FILE *cf = fopen(o->codeplug_path, "wb");
		if (cf) {
			fwrite(img, 1, len, cf);
			fclose(cf);
			printf("\n  codeplug saved to %s\n", o->codeplug_path);
		}
	}

	if (o->write_back && len)
		probe_write(img, len);
	else if (o->write_back)
		note("P-25", "write one record (its own bytes, unchanged)", R_SKIP,
		     "two ACKs, the second roughly 710 ms after the first",
		     "skipped: nothing was read, so there is nothing safe to write back");

	/* P-20 again at the end, after a whole session: the capture shows two idents 36 s apart. */
	{
		char a[MC_IDENT_MAX];
		size_t la = 0;
		/* Sequenced deliberately.  Calling mc_identify() inside note()'s argument list left the
		 * format string to be chosen from a `la` that had not been written yet -- C does not order
		 * argument evaluation -- and the first hardware run duly reported "as documented" beside
		 * "no reply". */
		int again = mc_identify(&R.s, a, sizeof a, &la);
		note("P-20", "ident still answered after a full session", again == 0 ? R_PASS : R_DIFFERS,
		     "the radio answers `*` at any point, not once per power-up",
		     again == 0 ? "answered again, %u bytes" : "no reply (%u bytes)", (unsigned)la);
	}

	if (R.trace)
		fclose(R.trace);
	mc_serial_close(t);
	write_report(o, pr, len, model);
	return 0;
}
