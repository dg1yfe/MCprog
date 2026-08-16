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
/* A transport backed by a captured trace -- spec.md section 5, testdata/traces/[*].trace.
 *
 * Every byte the implementation sends must equal the capture's next PC->radio byte.  That single
 * rule is what makes the subtle sequencing requirements testable: P-25's "never send block N+1
 * before the second ACK" needs no special assertion, because sending anything early lands on a
 * radio->PC event and fails at the exact byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/protocol.h"

struct mc_replay {
	mc_transport t; /* first, so a transport pointer casts back to this */
	char name[64];
	uint8_t *val;
	uint8_t *dir; /* 1 = PC->radio */
	unsigned *ts;
	size_t n, cap, pos;
	unsigned now;
};

static void push(mc_replay *r, uint8_t dir, uint8_t v, unsigned ts)
{
	if (r->n == r->cap) {
		r->cap = r->cap ? r->cap * 2 : 4096;
		r->val = realloc(r->val, r->cap);
		r->dir = realloc(r->dir, r->cap);
		r->ts = realloc(r->ts, r->cap * sizeof *r->ts);
	}
	r->val[r->n] = v;
	r->dir[r->n] = dir;
	r->ts[r->n] = ts;
	r->n++;
}

static int rp_send(mc_transport *t, const uint8_t *buf, size_t n)
{
	mc_replay *r = (mc_replay *)t;
	size_t i;
	for (i = 0; i < n; i++) {
		if (r->pos >= r->n) {
			snprintf(t->err, sizeof t->err, "%s: sent %02X past the end of the capture",
			         r->name, buf[i]);
			return -1;
		}
		if (!r->dir[r->pos]) {
			snprintf(t->err, sizeof t->err,
			         "%s: sent %02X at event %u, but the capture expects the radio to speak "
			         "next (%02X)",
			         r->name, buf[i], (unsigned)r->pos, r->val[r->pos]);
			return -1;
		}
		if (r->val[r->pos] != buf[i]) {
			snprintf(t->err, sizeof t->err, "%s: event %u: capture has %02X, we sent %02X",
			         r->name, (unsigned)r->pos, r->val[r->pos], buf[i]);
			return -1;
		}
		r->now = r->ts[r->pos];
		r->pos++;
	}
	return 0;
}

static int rp_recv(mc_transport *t, uint8_t *buf, size_t n, unsigned timeout_ms)
{
	mc_replay *r = (mc_replay *)t;
	size_t got = 0;
	while (got < n) {
		if (r->pos >= r->n || r->dir[r->pos])
			break; /* nothing more from the radio: a timeout, as far as the caller is concerned */
		if (r->ts[r->pos] > r->now + timeout_ms) {
			snprintf(t->err, sizeof t->err, "%s: event %u arrives %u ms late (limit %u)",
			         r->name, (unsigned)r->pos, r->ts[r->pos] - r->now, timeout_ms);
			break;
		}
		buf[got++] = r->val[r->pos];
		r->now = r->ts[r->pos];
		r->pos++;
	}
	return (int)got;
}

static unsigned rp_now(mc_transport *t)
{
	return ((mc_replay *)t)->now;
}

mc_replay *mc_replay_open(const char *path, char *err, size_t errsz)
{
	FILE *f = fopen(path, "rb");
	mc_replay *r;
	char line[4096];

	if (!f) {
		snprintf(err, errsz, "cannot open %s", path);
		return NULL;
	}
	r = calloc(1, sizeof *r);
	r->t.send = rp_send;
	r->t.recv = rp_recv;
	r->t.now_ms = rp_now;
	snprintf(r->name, sizeof r->name, "%s", path);

	while (fgets(line, sizeof line, f)) {
		char dir[4], hex[3200];
		unsigned seq, t0, t1;
		size_t nb, i;
		if (line[0] == '#')
			continue;
		if (sscanf(line, "TRACE %63s", r->name) == 1)
			continue;
		if (sscanf(line, "%3s %u %u %u %3199s", dir, &seq, &t0, &t1, hex) != 5)
			continue;
		nb = strlen(hex) / 2;
		for (i = 0; i < nb; i++) {
			unsigned v;
			unsigned ts = nb > 1 ? t0 + (unsigned)((t1 - t0) * i / (nb - 1)) : t0;
			sscanf(hex + i * 2, "%2x", &v);
			/* 0x00 and 0x01 appear in the PC stream at the start of a capture and again where
			 * the 2011 log changes phase.  They are line-state artifacts of the capture rig,
			 * not protocol -- no legal protocol byte is below 0x06 -- so they are dropped
			 * rather than demanded of the implementation. */
			if (dir[0] == 'T' && (v == 0x00 || v == 0x01))
				continue;
			push(r, (uint8_t)(dir[0] == 'T'), (uint8_t)v, ts);
		}
	}
	fclose(f);
	return r;
}

mc_transport *mc_replay_transport(mc_replay *r)
{
	return &r->t;
}

int mc_replay_exhausted(mc_replay *r)
{
	return r->pos >= r->n ? 0 : (int)r->pos;
}

const char *mc_replay_name(mc_replay *r)
{
	return r->name;
}

void mc_replay_close(mc_replay *r)
{
	free(r->val);
	free(r->dir);
	free(r->ts);
	free(r);
}
