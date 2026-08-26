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
/* Protocol conformance -- replays the captured sessions, citing spec.md requirement numbers.
 *
 *     ./test_protocol [repo-root]
 *
 * The replay transport compares every transmitted byte against the capture, so these tests assert
 * that the implementation reproduces what the 1987 software actually put on the wire -- not merely
 * that it is self-consistent.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/codeplug.h"
#include "mc/protocol.h"

static const char *ROOT = ".";
static int pass, fail;

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

static mc_replay *open_trace(const char *rel)
{
	char path[512], err[160];
	mc_replay *r;
	snprintf(path, sizeof path, "%s/%s", ROOT, rel);
	r = mc_replay_open(path, err, sizeof err);
	if (!r)
		failf("P-40", "%s", err);
	return r;
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

/* Pull the payloads of every `(40` record the PC transmitted in a trace, so a write replay can be
 * driven with exactly the bytes the original sent. */
static size_t written_records(const char *rel, uint8_t *out, size_t max)
{
	char path[512], line[4096];
	FILE *f;
	size_t off = 0;

	snprintf(path, sizeof path, "%s/%s", ROOT, rel);
	f = fopen(path, "rb");
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f) && off + MC_BLOCK <= max) {
		char dir[4], hex[3200];
		unsigned seq, t0, t1;
		uint8_t raw[8 + MC_BLOCK * 2];
		size_t nb, i;
		if (line[0] == '#' ||
		    sscanf(line, "%3s %u %u %u %3199s", dir, &seq, &t0, &t1, hex) != 5 || dir[0] != 'T')
			continue;
		nb = strlen(hex) / 2;
		if (nb != 7 + MC_BLOCK * 2 || nb > sizeof raw)
			continue;
		for (i = 0; i < nb; i++) {
			unsigned x;
			sscanf(hex + i * 2, "%2x", &x);
			raw[i] = (uint8_t)x;
		}
		if (raw[0] == '(' && mc_nib_decode(raw + 7, MC_BLOCK * 2, out + off) == 0)
			off += MC_BLOCK;
	}
	fclose(f);
	return off;
}

#define TRACE_2009 "testdata/traces/read_eva9_2009.trace"
#define TRACE_2011 "testdata/traces/read_eva9.trace"
#define TRACE_WRITE "testdata/traces/write_eva9.trace"

/* ---- P-3 framing, including the nibbles the captures leave constant ------------------------- */

static void test_header(void)
{
	char path[512], line[256];
	FILE *f;

	snprintf(path, sizeof path, "%s/testdata/proto/header.vec", ROOT);
	f = fopen(path, "rb");
	if (!f) {
		failf("P-3", "cannot open %s", path);
		return;
	}
	while (fgets(line, sizeof line, f)) {
		char cmd[8], want[32], got[32];
		unsigned addr;
		uint8_t hdr[7];
		int i;
		if (sscanf(line, "HDR cmd=%7s addr=%x want=%31s", cmd, &addr, want) != 3)
			continue;
		mc_put_header(hdr, cmd, (uint16_t)addr);
		for (i = 0; i < 7; i++)
			snprintf(got + i * 2, 3, "%02x", hdr[i]);
		if (strcmp(got, want) != 0)
			failf("P-3", "header %s %04x: want %s got %s", cmd, addr, want, got);
		else
			pass++;
	}
	fclose(f);
}

/* ---- the 2009 read: a clean session that must reconstruct a committed fixture ---------------- */

static void test_read_2009(void)
{
	mc_replay *r = open_trace(TRACE_2009);
	mc_session s;
	char ident[MC_IDENT_MAX];
	uint8_t img[1024], *ref;
	size_t ilen = 0, len = 0, reflen = 0;

	if (!r)
		return;
	mc_session_init(&s, mc_replay_transport(r));
	if (mc_connect(&s, ident, sizeof ident, &ilen) != 0) {
		failf("P-40", "connect: %s", s.err);
		mc_replay_close(r);
		return;
	}
	ok(ilen == 41, "P-20", "the ident is 41 bytes");
	ok(memcmp(ident, "EV9.01.00.11 455M11-3     5/6 Tone radio\x1a", 41) == 0, "P-20",
	   "the ident is the expected string, 0x1A-terminated");

	if (mc_read_all(&s, img, sizeof img, &len) != 0) {
		failf("P-41", "read_all: %s", s.err);
		mc_replay_close(r);
		return;
	}
	ok(len == 512, "P-41", "the size walk finds 512 bytes and stops at the header-NAK (P-24)");

	ref = slurp("fixtures/eva9_real.bin", &reflen);
	if (!ref) {
		failf("P-41", "cannot read fixtures/eva9_real.bin");
	} else {
		ok(reflen == len && memcmp(img, ref, len) == 0, "P-41",
		   "the replayed session reconstructs fixtures/eva9_real.bin byte for byte");
		free(ref);
	}
	/* The reconstruction feeds straight into the M1 layer, tying the milestones together. */
	{
		mc_image ci;
		ci.model = mc_model_by_name("eva_sel5");
		ci.bytes = img;
		ci.len = len;
		ok(mc_checksum_valid(&ci), "K-2", "and its checksum is valid");
		ok(mc_channel_count(&ci, NULL) == 24, "K-23", "and it decodes to 24 channels");
	}
	ok(mc_replay_exhausted(r) == 0, "P-41", "the whole capture was consumed");
	mc_replay_close(r);
}

/* ---- the 2011 read: same protocol, a radio holding an invalid codeplug ----------------------- */

static void test_read_2011(void)
{
	mc_replay *r = open_trace(TRACE_2011);
	mc_session s;
	char ident[MC_IDENT_MAX];
	uint8_t img[1024];
	size_t ilen = 0, len = 0;

	if (!r)
		return;
	mc_session_init(&s, mc_replay_transport(r));
	if (mc_connect(&s, ident, sizeof ident, &ilen) != 0) {
		failf("P-40", "connect: %s", s.err);
		mc_replay_close(r);
		return;
	}
	ok(ilen == 41, "P-20", "2011 capture: the ident is also 41 bytes");
	if (mc_read_all(&s, img, sizeof img, &len) != 0) {
		failf("P-41", "read_all: %s", s.err);
		mc_replay_close(r);
		return;
	}
	ok(len == 512, "P-41", "2011 capture: 512 bytes then the header-NAK");
	{
		mc_image ci;
		ci.model = mc_model_by_name("eva_sel5");
		ci.bytes = img;
		ci.len = len;
		/* A real radio returning an invalid checksum -- precisely why P-41 says report it and
		 * hand the data over rather than refusing it. */
		ok(!mc_checksum_valid(&ci), "P-41",
		   "this radio held an INVALID checksum and the read still succeeds");
	}
	mc_replay_close(r);
}

/* ---- the write session: the double ACK and per-record verification --------------------------- */

struct vcount {
	int n;
};

static int count_verify(void *ctx, uint16_t addr, const uint8_t *want, const uint8_t *got)
{
	(void)addr;
	((struct vcount *)ctx)->n++;
	return memcmp(want, got, MC_BLOCK) == 0 ? 0 : -1;
}

static void test_write(void)
{
	mc_replay *r = open_trace(TRACE_WRITE);
	mc_session s;
	uint8_t img[512];
	size_t len;
	uint8_t v = 0;
	struct vcount vc = { 0 };

	if (!r)
		return;
	len = written_records(TRACE_WRITE, img, sizeof img);
	ok(len == 512, "P-25", "the capture contains 8 written records");

	mc_session_init(&s, mc_replay_transport(r));
	if (mc_probe(&s, 0, &v) != 0) {
		failf("P-21", "probe: %s", s.err);
		mc_replay_close(r);
		return;
	}
	ok(v == 0x73, "P-21", "the probe returns one codeplug byte (0x73 here, 0x37 in 2009)");

	if (mc_write_all(&s, img, len, count_verify, &vc) != 0) {
		failf("P-25", "write_all: %s", s.err);
		mc_replay_close(r);
		return;
	}
	ok(vc.n == 8, "P-42", "all 8 records were written and read back");
	ok(s.last_burn_ms >= 700 && s.last_burn_ms <= 720, "P-25",
	   "the two ACKs are ~710 ms apart, so the burn really was awaited");
	ok(mc_replay_exhausted(r) == 0, "P-25", "the whole write capture was consumed");
	mc_replay_close(r);
}

/* ---- prove the suite catches the mistake P-25 exists to prevent ------------------------------ */

static void test_p25_is_enforced(void)
{
	mc_replay *r = open_trace(TRACE_WRITE);
	mc_transport *t;
	mc_session s;
	uint8_t img[512], frame[7 + MC_BLOCK * 2], a = 0, v = 0;

	if (!r)
		return;
	if (written_records(TRACE_WRITE, img, sizeof img) < MC_BLOCK) {
		mc_replay_close(r);
		return;
	}
	mc_session_init(&s, mc_replay_transport(r));
	mc_probe(&s, 0, &v);
	t = mc_replay_transport(r);

	/* Do exactly what a naive implementation does: send the record, take the first ACK as
	 * completion, and push on.  The capture must refuse the third step. */
	memcpy(frame, "(40", 3);
	frame[3] = frame[4] = frame[5] = frame[6] = 0x30;
	mc_nib_encode(img, MC_BLOCK, frame + 7);
	ok(t->send(t, frame, sizeof frame) == 0, "P-25", "the record itself is accepted");
	ok(t->recv(t, &a, 1, MC_T_BURN) == 1 && a == 0x06, "P-25", "the first ACK arrives");
	ok(t->send(t, (const uint8_t *)")400040", 7) != 0, "P-25",
	   "sending the next command before the second ACK is REJECTED by the capture");
	mc_replay_close(r);
}

/* P-27: mc_session_arm() must deliver exactly MC_ARM_PULSES, because the 1987 software was
 * measured doing exactly two -- and must be silent on a transport with no control lines, which is
 * every transport in this test suite and every replay of a capture. */
static int arm_calls;
static int counting_rearm(mc_transport *t) { (void)t; arm_calls++; return 0; }
static int failing_rearm(mc_transport *t)  { (void)t; arm_calls++; return -1; }

static void test_arm(void)
{
	mc_transport t;
	mc_session s;

	memset(&t, 0, sizeof t);
	mc_session_init(&s, &t);

	/* no hook: nothing happens, and it is not an error */
	arm_calls = 0;
	ok(mc_session_arm(&s) == 0, "P-27", "a transport without control lines arms zero times");
	ok(arm_calls == 0, "P-27", "  and the hook is not called at all");

	t.rearm = counting_rearm;
	arm_calls = 0;
	ok(mc_session_arm(&s) == MC_ARM_PULSES, "P-27", "a serial transport arms twice");
	ok(arm_calls == MC_ARM_PULSES, "P-27", "  calling the hook exactly that many times");
	ok(MC_ARM_PULSES == 2, "P-27", "  and twice is what the original was measured doing");

	/* a pulse that fails is counted as not delivered, but is not fatal */
	t.rearm = failing_rearm;
	arm_calls = 0;
	ok(mc_session_arm(&s) == 0, "P-27", "a failing pulse reports zero delivered");
	ok(arm_calls == MC_ARM_PULSES, "P-27", "  having still tried each time");

	/* arming means the radio restarted, so no acknowledgement can be owed across it */
	t.rearm = counting_rearm;
	s.pending_ack = 1;
	mc_session_arm(&s);
	ok(s.pending_ack == 0, "P-27", "arming clears any pending acknowledgement");
}

int main(int argc, char **argv)
{
	if (argc > 1)
		ROOT = argv[1];
	test_header();
	test_read_2009();
	test_read_2011();
	test_write();
	test_p25_is_enforced();
	test_arm();
	printf("\n%d passed, %d FAILED\n", pass, fail);
	return fail ? 1 : 0;
}
