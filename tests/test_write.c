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
/* The write path -- spec.md section 8 (W-n).
 *
 * Every gate is asserted by making it fire, because a gate that has never refused anything is not
 * known to work.  The radio is the forked fake over a pty, so a refusal is observable two ways: the
 * call returns -1, and the radio's own copy of the EEPROM is unchanged afterwards.
 *
 * These tests write files into a scratch directory and chdir there first, since a write leaves a
 * backup in the working directory by design.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif
#include "mc/codeplug.h"
#include "mc/serial.h"
#include "mc/write.h"

static const char *ROOT = ".";
static int pass, fail;
static const char IDENT[] = "EV9.01.00.11 455M11-3     5/6 Tone radio\x1a";
#define IDENTLEN 41
#define CHILD_DUMP "/tmp/mc_write_eeprom.bin"
#define SCRATCH "/tmp/mc_write_test"

static void ok(int cond, const char *req, const char *what)
{
	if (cond) {
		pass++;
	} else {
		fail++;
		printf("  FAIL [%s] %s\n", req, what);
	}
}

static void failf(const char *req, const char *fmt, ...)
{
	va_list ap;
	fail++;
	printf("  FAIL [%s] ", req);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static uint8_t *slurp(const char *rel, size_t *len)
{
	char path[512];
	FILE *f;
	long n;
	uint8_t *b;

	snprintf(path, sizeof path, "%s/%s", ROOT, rel);
	f = fopen(path, "rb");
	if (!f) {
		failf("W-0", "cannot open %s", path);
		return NULL;
	}
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

/* ---- the fake radio on the other end of a pty ------------------------------------------------ */

struct radio {
	pid_t pid;
	int master;
	mc_transport *t;
	mc_session s;
};

static int start_radio(struct radio *r, const uint8_t *image, size_t len)
{
	int master, slave, ready[2];
	char go, err[160];
	mc_serial_opts o;

	remove(CHILD_DUMP);
	if (openpty(&master, &slave, NULL, NULL, NULL) != 0)
		return -1;
	if (pipe(ready) != 0)
		return -1;
	r->pid = fork();
	if (r->pid < 0)
		return -1;
	if (r->pid == 0) {
		mc_fake f;
		mc_transport *t;
		uint8_t *eep = malloc(len);
		FILE *out;

		close(master);
		close(ready[0]);
		memcpy(eep, image, len);
		mc_serial_defaults(&o);
		o.baud = 0;
		o.line_setup = 0;
		t = mc_serial_attach(slave, &o, err, sizeof err);
		if (!t)
			_exit(2);
		memset(&f, 0, sizeof f);
		f.eep = eep;
		f.len = len;
		f.ident = IDENT;
		f.identlen = IDENTLEN;
		f.burn_ms = 5; /* P-25 shape without P-25 duration; the timing itself is test_serial's job */
		if (write(ready[1], "1", 1) != 1)
			_exit(3);
		close(ready[1]);
		mc_fake_serve(&f, t, 1500);
		out = fopen(CHILD_DUMP, "wb");
		if (out) {
			fwrite(eep, 1, len, out);
			fclose(out);
		}
		_exit(0);
	}
	close(slave);
	close(ready[1]);
	if (read(ready[0], &go, 1) != 1)
		return -1;
	close(ready[0]);
	mc_serial_defaults(&o);
	o.baud = 0;
	o.line_setup = 0;
	r->master = master;
	r->t = mc_serial_attach(master, &o, err, sizeof err);
	if (!r->t)
		return -1;
	mc_session_init(&r->s, r->t);
	return 0;
}

/* Stop the radio and return what its EEPROM held, so a refusal can be shown to have left the radio
 * alone rather than merely to have reported that it did. */
static uint8_t *stop_radio(struct radio *r, size_t *len)
{
	uint8_t *got;
	FILE *f;
	int st;

	mc_serial_close(r->t);
	waitpid(r->pid, &st, 0); /* the child exits once the pty closes or it idles out */
	f = fopen(CHILD_DUMP, "rb");
	if (!f) {
		*len = 0;
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	got = malloc(*len);
	if (fread(got, 1, *len, f) != *len) {
		free(got);
		got = NULL;
		*len = 0;
	}
	fclose(f);
	return got;
}

/* Try to write `img` to a radio holding `radio_image`.  Fills `rep`; returns mc_write_radio's
 * result.  `unchanged` reports whether the radio's copy survived untouched. */
static int attempt(const mc_image *img, const uint8_t *radio_image, size_t rlen,
                   mc_write_report *rep, int *unchanged)
{
	struct radio r;
	uint8_t *after;
	size_t alen;
	int rc;

	if (start_radio(&r, radio_image, rlen) != 0) {
		failf("W-0", "could not start the fake radio");
		return -99;
	}
	rc = mc_write_radio(&r.s, img, rep);
	after = stop_radio(&r, &alen);
	if (unchanged)
		*unchanged = after && alen == rlen && memcmp(after, radio_image, rlen) == 0;
	free(after);
	return rc;
}

static void fixsum(mc_image *img)
{
	mc_checksum_fix(img);
}

/* Move channel 1's transmit frequency by one 25 kHz step -- a change any user might make, and the
 * smallest one that touches a frequency field. */
static void bump_tx(mc_image *img)
{
	mc_channel c;
	mc_channel_get(img, 0, mc_band_p(mc_band_index(img)), &c);
	if (mc_channel_set_freq(img, 0, MC_TX, c.tx_hz + 25000) != 0)
		failf("K-10", "channel 1 + 25 kHz is not representable");
}

/* ---- W-3: the gates ------------------------------------------------------------------------- */

static void test_gates(void)
{
	uint8_t *real, *dflt;
	size_t rlen, dlen;
	const mc_model *m = mc_model_by_name("eva_56");
	mc_write_report rep;
	mc_image img;
	uint8_t buf[512];
	int rc, untouched;

	real = slurp("fixtures/eva9_real.bin", &rlen);
	dflt = slurp("fixtures/ev9_default.bin", &dlen);
	if (!real || !dflt)
		return;

	/* nothing to do -- a write that would change no byte is refused, not performed */
	memcpy(buf, real, rlen);
	img.model = m;
	img.bytes = buf;
	img.len = rlen;
	rc = attempt(&img, real, rlen, &rep, &untouched);
	ok(rc != 0, "W-3", "an identical codeplug is refused");
	ok(strstr(rep.err, "already holds") != NULL, "W-3", "and says why");
	ok(untouched, "W-3", "the radio is untouched");
	ok(rep.backup[0] == 0, "W-2", "a refusal before the first write byte leaves no backup behind");

	/* invalid checksum */
	memcpy(buf, real, rlen);
	buf[0x0E3] = 0x44; /* a channel edit, checksum deliberately not fixed */
	rc = attempt(&img, real, rlen, &rep, &untouched);
	ok(rc != 0 && strstr(rep.err, "checksum") != NULL, "W-3", "an invalid checksum is refused");
	ok(untouched, "W-3", "the radio is untouched after a bad checksum");

	/* band 7: the factory default has no band programmed, so it cannot be written to a radio */
	if (dlen == rlen) {
		memcpy(buf, dflt, dlen);
		img.len = dlen;
		rc = attempt(&img, real, rlen, &rep, &untouched);
		ok(rc != 0 && strstr(rep.err, "band") != NULL, "W-3",
		   "an unprogrammed band (7) is refused");
		ok(untouched, "W-3", "the radio is untouched after a band-7 refusal");
	} else {
		failf("W-3", "ev9_default.bin is %u bytes, expected %u", (unsigned)dlen, (unsigned)rlen);
	}

	/* size mismatch: a 256-byte codeplug against a 512-byte radio */
	{
		uint8_t small[256];
		mc_image si;
		size_t elen;
		uint8_t *eza = slurp("fixtures/eza9_default_band2.bin", &elen);
		if (eza && elen == 256) {
			memcpy(small, eza, 256);
			si.model = mc_model_by_name("eza_sel5");
			si.bytes = small;
			si.len = 256;
			rc = attempt(&si, real, rlen, &rep, &untouched);
			ok(rc != 0 && strstr(rep.err, "bytes") != NULL, "W-3",
			   "a codeplug of the wrong size is refused");
			ok(untouched, "W-3", "the radio is untouched after a size mismatch");
		}
		free(eza);
	}

	/* K-30: a byte that differs and is not one this program writes */
	memcpy(buf, real, rlen);
	img.len = rlen;
	buf[m->refdiv] ^= 0xFF; /* a reference divider -- preserved verbatim, never ours to change */
	fixsum(&img);
	rc = attempt(&img, real, rlen, &rep, &untouched);
	ok(rc != 0 && strstr(rep.err, "K-30") != NULL, "K-30",
	   "an unaccountable difference is refused");
	{
		char off[16];
		snprintf(off, sizeof off, "0x%03X", m->refdiv);
		ok(strstr(rep.err, off) != NULL, "K-30", "and the offset is named");
	}
	ok(untouched, "K-30", "the radio is untouched after a K-30 refusal");

	free(real);
	free(dflt);
}

/* ---- W-2, W-4, W-6: the write that goes through ----------------------------------------------- */

static void test_write_through(void)
{
	uint8_t *real;
	size_t rlen, blen = 0;
	const mc_model *m = mc_model_by_name("eva_56");
	mc_write_report rep;
	mc_image img;
	uint8_t buf[512], *backup, *after;
	struct radio r;
	int rc;
	uint32_t f_before, f_after, f_edit;

	real = slurp("fixtures/eva9_real.bin", &rlen);
	if (!real)
		return;
	memcpy(buf, real, rlen);
	img.model = m;
	img.bytes = buf;
	img.len = rlen;

	{
		mc_channel c;
		mc_channel_get(&img, 0, mc_band_p(mc_band_index(&img)), &c);
		f_before = c.rx_hz;
		f_edit = c.tx_hz + 25000; /* one channel, moved 25 kHz */
	}
	ok(mc_channel_set_freq(&img, 0, MC_TX, f_edit) == 0, "K-10", "the edit is representable");
	fixsum(&img);

	if (start_radio(&r, real, rlen) != 0) {
		failf("W-0", "could not start the fake radio");
		free(real);
		return;
	}
	rc = mc_write_radio(&r.s, &img, &rep);
	after = stop_radio(&r, &blen);

	ok(rc == 0, "W-4", "a legitimate edit is written");
	if (rc != 0)
		printf("       (%s)\n", rep.err);
	ok(rep.records == (int)(rlen / MC_BLOCK), "W-6", "every record is written");
	ok(rep.changed > 0 && rep.changed < 8, "W-3", "only the edited bytes and the checksum differ");
	ok(after && blen == rlen && memcmp(after, buf, rlen) == 0, "W-4",
	   "the radio ends up holding exactly the codeplug");

	/* W-2: the backup is the radio's contents from before the write, not after */
	backup = NULL;
	if (rep.backup[0]) {
		FILE *f = fopen(rep.backup, "rb");
		if (f) {
			backup = malloc(rlen);
			if (fread(backup, 1, rlen, f) != rlen) {
				free(backup);
				backup = NULL;
			}
			fclose(f);
		}
	}
	ok(backup != NULL, "W-2", "a backup file is written");
	ok(backup && memcmp(backup, real, rlen) == 0, "W-2",
	   "and holds what the radio had before the write");
	if (rep.backup[0])
		remove(rep.backup);
	free(backup);

	/* the edit is the one that was asked for, and nothing else moved */
	{
		mc_image w;
		mc_channel c;
		w.model = m;
		w.bytes = after;
		w.len = blen;
		if (after && blen == rlen) {
			mc_channel_get(&w, 0, mc_band_p(mc_band_index(&w)), &c);
			f_after = c.rx_hz;
			ok(f_after == f_before, "K-9", "the untouched RX field is unchanged");
			ok(c.tx_hz == f_edit, "K-10",
			   "the edited TX field reads back as the frequency that was asked for");
			ok(mc_checksum_valid(&w), "K-2", "the radio's copy has a valid checksum");
		}
	}
	free(after);
	free(real);
}

/* K-15 through the write path: an edited auto-acknowledge delay must be recognised as a byte
 * MCprog writes (K-30), or W-3 would refuse the write it just asked the user to confirm. */
static void test_write_aak(void)
{
	uint8_t *eza;
	size_t elen;
	const mc_model *m = mc_model_by_name("eza_sel5");
	mc_write_report rep;
	mc_image img;
	uint8_t buf[256];
	int rc, untouched;

	eza = slurp("fixtures/eza9_default_band2.bin", &elen);
	if (!eza || elen != 256)
		return;
	memcpy(buf, eza, elen);
	img.model = m;
	img.bytes = buf;
	img.len = elen;

	ok(mc_aak_set_ms(&img, 1000) == 0, "K-15", "the delay is editable on this model");
	fixsum(&img);
	rc = attempt(&img, eza, elen, &rep, &untouched);
	ok(rc == 0, "K-30", "and writing the result is not refused as unaccountable");
	if (rc != 0)
		printf("       (%s)\n", rep.err);
	else
		ok(rep.changed == 2, "W-3", "exactly the delay byte and the checksum changed");
	if (rep.backup[0])
		remove(rep.backup);
	free(eza);
}

/* W-2: if the backup cannot be written, nothing is written to the radio either. */
static void test_backup_required(void)
{
	uint8_t *real;
	size_t rlen;
	mc_write_report rep;
	mc_image img;
	uint8_t buf[512];
	int rc, untouched;
	char cwd[512];

	if (geteuid() == 0) {
		printf("  (skipped: running as root, an unwritable directory would still be writable)\n");
		return;
	}
	real = slurp("fixtures/eva9_real.bin", &rlen);
	if (!real)
		return;
	memcpy(buf, real, rlen);
	img.model = mc_model_by_name("eva_56");
	img.bytes = buf;
	img.len = rlen;
	bump_tx(&img);
	fixsum(&img);

	if (!getcwd(cwd, sizeof cwd)) {
		free(real);
		return;
	}
	mkdir(SCRATCH "/ro", 0700);
	if (chdir(SCRATCH "/ro") != 0 || chmod(".", 0500) != 0) {
		if (chdir(cwd) != 0)
			failf("W-2", "could not return to the working directory");
		free(real);
		return;
	}
	rc = attempt(&img, real, rlen, &rep, &untouched);
	if (chmod(SCRATCH "/ro", 0700) != 0 || chdir(cwd) != 0)
		failf("W-2", "could not restore the working directory");

	ok(rc != 0, "W-2", "a backup that cannot be written stops the write");
	ok(strstr(rep.err, "backup") != NULL, "W-2", "and says so");
	ok(untouched, "W-2", "the radio is untouched when the backup fails");
	free(real);
}

/* ---- K-11: what counts as a difference in a read-back ---------------------------------------- */

static void test_verify_rule(void)
{
	uint8_t *real;
	size_t rlen;
	const mc_model *m = mc_model_by_name("eva_56");
	mc_image img;
	uint8_t want[MC_BLOCK], got[MC_BLOCK];
	unsigned p = mc_band_p(2); /* 80 */
	char err[200];
	size_t base = m->chan + m->tx; /* channel 0's TX field */
	uint16_t addr = (uint16_t)(base & ~(size_t)(MC_BLOCK - 1));
	size_t k = base - addr;

	real = slurp("fixtures/eva9_real.bin", &rlen);
	if (!real)
		return;
	img.model = m;
	img.bytes = real;
	img.len = rlen;

	memcpy(want, real + addr, MC_BLOCK);
	memcpy(got, want, MC_BLOCK);
	ok(mc_write_verify_record(&img, p, addr, want, got, err, sizeof err) == 0, "W-4",
	   "an identical read-back is accepted");

	/* K-11: the same frequency spelled differently.  Subtracting one from the coarse count and
	 * adding P to the offset denotes exactly the same frequency; the encoder never emits this, but
	 * two fields in the sample codeplugs do, so verification must not reject it. */
	if (mc_freq_decode(want + k, p) > 0 && (want[k + 1] > 0 || (want[k] & 3))) {
		unsigned f = mc_freq_decode(want + k, p);
		got[k + 1] = (uint8_t)(want[k + 1] - 1);
		got[k + 2] = (uint8_t)(want[k + 2] + p);
		if (mc_freq_decode(got + k, p) == f) {
			ok(mc_write_verify_record(&img, p, addr, want, got, err, sizeof err) == 0, "K-11",
			   "an alternate spelling of the same frequency is accepted");
		} else {
			failf("K-11", "the constructed alternate spelling decodes to %u, not %u",
			      mc_freq_decode(got + k, p), f);
		}
		/* one step further is a different frequency, and must be rejected */
		memcpy(got, want, MC_BLOCK);
		got[k + 2] = (uint8_t)(want[k + 2] + 1);
		ok(mc_write_verify_record(&img, p, addr, want, got, err, sizeof err) != 0, "W-4",
		   "a frequency that came back different is rejected");
	} else {
		failf("K-11", "channel 0 of eva9_real.bin cannot express an alternate spelling");
	}

	/* a byte outside any frequency field is compared exactly */
	memcpy(got, want, MC_BLOCK);
	got[0] ^= 0x01;
	ok(mc_write_verify_record(&img, p, addr, want, got, err, sizeof err) != 0, "W-4",
	   "a non-frequency byte that came back different is rejected");
	ok(strstr(err, "0x0") != NULL || err[0], "W-4", "and the record is named");
	free(real);
}

/* ---- mc_write_explain, used to preview a write before confirming it -------------------------- */

static void test_explain(void)
{
	uint8_t *real;
	size_t rlen;
	const mc_model *m = mc_model_by_name("eva_56");
	mc_image img;
	uint8_t buf[512];
	char why[220];
	int n;

	real = slurp("fixtures/eva9_real.bin", &rlen);
	if (!real)
		return;
	memcpy(buf, real, rlen);
	img.model = m;
	img.bytes = buf;
	img.len = rlen;

	n = mc_write_explain(&img, real, rlen, why, sizeof why);
	ok(n == 0, "W-3", "an unmodified codeplug differs from the radio in no bytes");

	bump_tx(&img);
	fixsum(&img);
	n = mc_write_explain(&img, real, rlen, why, sizeof why);
	ok(n > 0 && n <= 4, "W-3", "one channel edit accounts for a handful of bytes");
	ok(why[0] == 0, "W-3", "and needs no explanation");

	buf[m->band] ^= 0x0F; /* the band byte's low nibble is preserved verbatim, never ours */
	fixsum(&img);
	ok(mc_write_explain(&img, real, rlen, why, sizeof why) < 0, "K-30",
	   "a change outside our fields is reported, not counted");
	free(real);
}

int main(int argc, char **argv)
{
	char cwd[512];

	if (argc > 1)
		ROOT = argv[1];
	if (ROOT[0] != '/' && getcwd(cwd, sizeof cwd)) {
		static char abs[1024];
		snprintf(abs, sizeof abs, "%s/%s", cwd, ROOT);
		ROOT = abs;
	}
	/* every write leaves a backup in the working directory, so run somewhere disposable */
	mkdir(SCRATCH, 0700);
	if (chdir(SCRATCH) != 0) {
		printf("cannot use %s as a scratch directory\n", SCRATCH);
		return 1;
	}
	signal(SIGCHLD, SIG_DFL);

	printf("write (spec.md section 8)\n");
	test_gates();
	test_write_through();
	test_write_aak();
	test_backup_required();
	test_verify_rule();
	test_explain();

	printf("%d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
