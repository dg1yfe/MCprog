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
/* Writing a codeplug to a radio -- spec.md section 8.
 *
 * W-5, the write counter, IS implemented, and only for radio writes: the bytes handed to the radio
 * carry a bumped counter and a recomputed checksum, while the caller's image is left exactly as it
 * was, so saving a file never advances it.  The law is not an eight-bit increment -- see
 * mc_write_counter_next in codeplug.c.  Models with no measured counter (the 5/6-tone build, which
 * keeps a date there instead, and MCEZ13) are written unchanged.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mc/write.h"

/* Is `off` a byte MCprog could legitimately have changed?  Everything else differing means the
 * image and the radio disagree about something we do not understand, and K-30 says refuse. */
static int ours(const mc_model *m, size_t off)
{
	if (off == m->cksum)
		return 1;
	if (off >= m->chan && off < (size_t)m->chan + (size_t)m->nchan * m->stride)
		return 1;
	if (m->pl_list && off >= m->pl_list && off < (size_t)m->pl_list + (size_t)m->pl_max * 2)
		return 1;
	if (m->pl_tone && off >= m->pl_tone && off < (size_t)m->pl_tone + 2)
		return 1;
	if (m->pl_dec && off >= m->pl_dec && off < (size_t)m->pl_dec + (size_t)m->pl_max * 2)
		return 1;
	if (m->pl_count && off == m->pl_count)
		return 1;
	if (m->aak && off == m->aak)
		return 1;
	if (m->pl_mode && off == m->pl_mode)
		return 1;
	if (m->wcount && off == m->wcount)   /* W-5: we are about to bump it anyway */
		return 1;
	return 0;
}

int mc_write_explain(const mc_image *img, const uint8_t *radio, size_t radio_len,
                     char *why, size_t whysz)
{
	size_t i, n = img->len < radio_len ? img->len : radio_len;
	int changed = 0;

	if (why && whysz)
		why[0] = 0;
	for (i = 0; i < n; i++) {
		if (img->bytes[i] == radio[i])
			continue;
		changed++;
		if (!ours(img->model, i)) {
			snprintf(why, whysz,
			         "0x%03X differs (radio %02X, ours %02X) and is not a byte this program "
			         "writes -- refusing rather than guessing (K-30)",
			         (unsigned)i, radio[i], img->bytes[i]);
			return -1;
		}
	}
	return changed;
}

/* K-11: a frequency field has more than one legal spelling, so verification compares decoded
 * frequencies.  Comparing bytes would reject a correct write. */
struct vctx {
	const mc_image *img;
	unsigned p;
	char err[200];
};

static int freq_equal(const uint8_t *a, const uint8_t *b, unsigned p)
{
	return mc_freq_decode(a, p) == mc_freq_decode(b, p);
}

static int in_freq_field(const mc_model *m, size_t off, size_t *base)
{
	size_t rel, slot, within;
	if (off < m->chan || off >= (size_t)m->chan + (size_t)m->nchan * m->stride)
		return 0;
	rel = off - m->chan;
	slot = rel / m->stride;
	within = rel % m->stride;
	if (within >= m->tx && within < (size_t)m->tx + 3) {
		*base = m->chan + slot * m->stride + m->tx;
		return 1;
	}
	if (within >= m->rx && within < (size_t)m->rx + 3) {
		*base = m->chan + slot * m->stride + m->rx;
		return 1;
	}
	return 0;
}

int mc_write_verify_record(const mc_image *img, unsigned p, uint16_t addr, const uint8_t *want,
                           const uint8_t *got, char *err, size_t errsz)
{
	size_t i;

	if (err && errsz)
		err[0] = 0;
	for (i = 0; i < MC_BLOCK; i++) {
		size_t off = (size_t)addr + i, base;
		if (want[i] == got[i])
			continue;
		/* the differing byte may be a legal alternate spelling of the same frequency */
		if (p && in_freq_field(img->model, off, &base) && base >= addr &&
		    base + 2 < (size_t)addr + MC_BLOCK) {
			size_t k = base - addr;
			if (freq_equal(want + k, got + k, p))
				continue;
		}
		snprintf(err, errsz, "record 0x%04X byte +%u: wrote %02X, radio returned %02X", addr,
		         (unsigned)i, want[i], got[i]);
		return -1;
	}
	return 0;
}

static int verify_record(void *ctx, uint16_t addr, const uint8_t *want, const uint8_t *got)
{
	struct vctx *v = ctx;
	return mc_write_verify_record(v->img, v->p, addr, want, got, v->err, sizeof v->err);
}

/* Every gate below runs before the first write byte, so a refusal means the radio is untouched and
 * the backup is just clutter.  Remove it and say so; keep it the moment writing begins. */
static int refuse(mc_write_report *rep, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(rep->err, sizeof rep->err, fmt, ap);
	va_end(ap);
	if (rep->backup[0]) {
		remove(rep->backup);
		rep->backup[0] = 0;
	}
	return -1;
}

int mc_write_radio(mc_session *s, const mc_image *img, const char *backup_path,
                   mc_write_report *rep)
{
	static uint8_t radio[MC_IMG_MAX], out[MC_IMG_MAX];
	mc_image sent;
	size_t rlen = 0;
	struct vctx v;
	FILE *f;
	time_t now;
	struct tm *tm;
	char stamp[64];
	int changed;
	char why[220];

	memset(rep, 0, sizeof *rep);

	/* W-2: know what is there before changing it. */
	if (mc_read_all(s, radio, sizeof radio, &rlen) != 0) {
		snprintf(rep->err, sizeof rep->err, "pre-write read failed: %s", s->err);
		return -1;
	}
	rep->radio_len = rlen;
	if (backup_path && *backup_path) {
		snprintf(rep->backup, sizeof rep->backup, "%s", backup_path);
		f = fopen(rep->backup, "wb");
	} else {
		/* The generated name has one-second resolution, so two writes in the same second would
		 * land on one file.  Never clobber a backup -- that is the one file W-2 exists to keep --
		 * so try suffixes until one does not exist yet. */
		int n;
		now = time(NULL);
		tm = localtime(&now);
		strftime(stamp, sizeof stamp, "mcprog-backup-%Y%m%d-%H%M%S", tm);
		for (n = 0, f = NULL; n < 100; n++) {
			FILE *probe;
			if (n)
				snprintf(rep->backup, sizeof rep->backup, "%s-%d.dat", stamp, n);
			else
				snprintf(rep->backup, sizeof rep->backup, "%s.dat", stamp);
			probe = fopen(rep->backup, "rb");
			if (probe) {
				fclose(probe);
				continue;
			}
			f = fopen(rep->backup, "wb");
			break;
		}
	}
	if (!f || fwrite(radio, 1, rlen, f) != rlen) {
		if (f)
			fclose(f);
		snprintf(rep->err, sizeof rep->err,
		         "could not write the backup %s -- refusing to write the radio", rep->backup);
		rep->backup[0] = 0;
		return -1;
	}
	fclose(f);

	/* W-3: the gates, all fatal, all before the first write byte. */
	if (rlen != img->len) {
		return refuse(rep,
		              "the radio returned %u bytes but this codeplug is %u -- different model or "
		              "EEPROM size", (unsigned)rlen, (unsigned)img->len);
	}
	if (mc_image_check(img)) {
		return refuse(rep, "the codeplug is shorter than model %s addresses", img->model->name);
	}
	if (!mc_checksum_valid(img)) {
		return refuse(rep, "the codeplug's checksum is invalid -- fix it before writing");
	}
	if (mc_band_index(img) == 7) {
		return refuse(rep,
		              "the band is unprogrammed (index 7); the radio would have no usable frequencies");
	}
	changed = mc_write_explain(img, radio, rlen, why, sizeof why);
	if (changed < 0) {
		return refuse(rep, "%s", why);
	}
	rep->changed = changed;
	if (changed == 0) {
		return refuse(rep, "the radio already holds exactly this codeplug");
	}

	/* W-5: the radio's copy records having been reprogrammed; the caller's image does not.  The
	 * bump happens after every gate has passed, so a refused write never advances it. */
	memcpy(out, img->bytes, img->len);
	sent = *img;
	sent.bytes = out;
	if (mc_write_counter_bump(&sent) == 0) {
		mc_checksum_fix(&sent);
		rep->counter = 1;
	}

	/* W-4, W-6: every record in order, each read back and compared. */
	v.img = &sent;
	v.p = mc_band_p(mc_band_index(img));
	v.err[0] = 0;
	if (mc_write_all(s, sent.bytes, sent.len, verify_record, &v) != 0) {
		/* The backup path leads, because after a half-written EEPROM it is the only thing the user
		 * strictly needs from this sentence. */
		snprintf(rep->err, sizeof rep->err,
		         "the write stopped part way; the radio's previous contents are in %s -- %s%s%s",
		         rep->backup, s->err, v.err[0] ? " -- " : "", v.err);
		return -1;
	}
	rep->records = (int)(img->len / MC_BLOCK);
	return 0;
}
