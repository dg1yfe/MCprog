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
/* The terminal interface -- see ../../spec.md, section 7 (U-n).
 *
 * The channel list is one row per channel with a visible marker where the table terminates and
 * stale rows dimmed below it (U-1, K-23); selecting a channel opens a page with every field it has
 * and a line of help (U-2).  Validation never clamps: a frequency the radio cannot represent is
 * refused with a reason (U-3).
 *
 * All codeplug logic lives in the library and all argument handling in main.c; this file only
 * draws and asks.
 */
#include <ctype.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include "mc/codeplug.h"
#include "mc/tui.h"

static uint8_t orig_bytes[MC_IMG_MAX];
/* Shown on the status line until the first keypress -- which is then acted on, not swallowed. */
static const char *opening_note;
static mc_image img;
static char path[512];
static int dirty;

/* ---- helpers -------------------------------------------------------------------------------- */

static void fmt_mhz(char *dst, size_t n, uint32_t hz)
{
	snprintf(dst, n, "%u.%05u", hz / 1000000u, (hz % 1000000u) / 10u);
}

/* Accepts "145", "145.0", "145.17500"; returns 0 on success. */
static int parse_mhz(const char *s, uint32_t *hz)
{
	unsigned long mhz = 0, frac = 0;
	int digits = 0, seen = 0;

	while (isspace((unsigned char)*s))
		s++;
	if (!isdigit((unsigned char)*s))
		return -1;
	while (isdigit((unsigned char)*s)) {
		mhz = mhz * 10 + (unsigned long)(*s++ - '0');
		seen = 1;
	}
	if (*s == '.' || *s == ',') {
		s++;
		while (isdigit((unsigned char)*s)) {
			if (digits < 6) {
				frac = frac * 10 + (unsigned long)(*s - '0');
				digits++;
			}
			s++;
		}
	}
	while (isspace((unsigned char)*s))
		s++;
	if (*s || !seen || mhz > 4000)
		return -1;
	while (digits++ < 6)
		frac *= 10;
	*hz = (uint32_t)(mhz * 1000000UL + frac);
	return 0;
}

static const char *state_word(mc_chan_state st)
{
	switch (st) {
	case MC_CH_EMPTY: return "unprogrammed";
	case MC_CH_STALE: return "stale";
	default: return "";
	}
}

/* A compact flag summary: an upper-case initial per set flag. */
static void flag_summary(char *dst, size_t n, int slot0)
{
	const mc_flag *f;
	size_t nf, i, o = 0;
	f = mc_flags(img.model, &nf);
	for (i = 0; i < nf && o + 2 < n; i++)
		if (mc_flag_get(&img, slot0, &f[i]))
			dst[o++] = (char)toupper((unsigned char)f[i].name[0]);
	dst[o] = 0;
}

static void status(const char *msg)
{
	int y = LINES - 1;
	move(y, 0);
	clrtoeol();
	attron(A_REVERSE);
	mvprintw(y, 0, " %-*s", COLS - 2, msg);
	attroff(A_REVERSE);
}

/* Prompt on the bottom line.  Returns 0 if the user entered something, -1 on escape. */
static int prompt(const char *label, char *buf, size_t n)
{
	int y = LINES - 1, rc;
	move(y, 0);
	clrtoeol();
	mvprintw(y, 0, "%s", label);
	echo();
	curs_set(1);
	rc = getnstr(buf, (int)n - 1);
	curs_set(0);
	noecho();
	move(y, 0);
	clrtoeol();
	return rc == ERR ? -1 : 0;
}

/* ---- the channel list, U-1 ------------------------------------------------------------------ */

static void draw_list(int sel, int top)
{
	int band = mc_band_index(&img);
	unsigned p = mc_band_p(band);
	int term = 0, live = mc_channel_count(&img, &term);
	int rows = LINES - 5, i;
	char buf[128];

	erase();
	attron(A_BOLD);
	mvprintw(0, 0, "%s", path[0] ? path : "(read from radio, not yet saved)");
	attroff(A_BOLD);
	if (p)
		snprintf(buf, sizeof buf, "model %s  %u B  band %d (P=%u, %u Hz steps)  checksum %s%s",
		         img.model->name, (unsigned)img.len, band, p, mc_step_hz(&img),
		         mc_checksum_valid(&img) ? "ok" : "INVALID", dirty ? "  [modified]" : "");
	else
		snprintf(buf, sizeof buf,
		         "model %s  %u B  band 7 = UNPROGRAMMED, frequencies unavailable  checksum %s",
		         img.model->name, (unsigned)img.len, mc_checksum_valid(&img) ? "ok" : "INVALID");
	mvprintw(1, 0, "%s", buf);
	attron(A_UNDERLINE);
	mvprintw(3, 0, "%-4s %-4s %-12s %-12s %-8s %s", "slot", "num", "TX (MHz)", "RX (MHz)", "flags",
	         "state");
	attroff(A_UNDERLINE);

	for (i = 0; i < rows && top + i < img.model->nchan; i++) {
		int slot0 = top + i;
		mc_channel c;
		char tx[24], rx[24], fl[16], num[8];
		int y = 4 + i;

		mc_channel_get(&img, slot0, p, &c);
		if (img.model->numbered)
			snprintf(num, sizeof num, "%02X", c.num);
		else
			snprintf(num, sizeof num, "--");
		if (c.state == MC_CH_STALE)
			fl[0] = 0;
		else
			flag_summary(fl, sizeof fl, slot0);

		/* K-24: an unprogrammed slot is not 0.00000 MHz, and neither is a stale record -- its
		 * bytes are leftovers that were never decoded.  Both print as "--". */
		if (!p || c.state == MC_CH_EMPTY || c.state == MC_CH_STALE) {
			snprintf(tx, sizeof tx, "%s", "--");
			snprintf(rx, sizeof rx, "%s", "--");
		} else {
			fmt_mhz(tx, sizeof tx, c.tx_hz);
			fmt_mhz(rx, sizeof rx, c.rx_hz);
		}
		if (slot0 == sel)
			attron(A_REVERSE);
		if (c.state == MC_CH_STALE)
			attron(A_DIM);
		mvprintw(y, 0, "%-4d %-4s %-12s %-12s %-8s %s", slot0 + 1, num, tx, rx, fl,
		         (term && slot0 == live) ? "terminator -- table ends here" : state_word(c.state));
		if (c.state == MC_CH_STALE)
			attroff(A_DIM);
		if (slot0 == sel)
			attroff(A_REVERSE);
	}
	status(opening_note ? opening_note
	                    : mc_pl_supported(img.model)
	                          ? "up/down or j/k   enter edit   p PL/CTCSS   s save   q quit"
	                          : "up/down or j/k   enter edit   s save   q quit");
	refresh();
}

/* ---- the per-channel page, U-2 -------------------------------------------------------------- */

static const char *flag_help(const mc_flag *f)
{
	if (!strcmp(f->name, "clock_shift"))
		return "shifts the CPU clock slightly so its harmonics miss the receive frequency";
	if (!strcmp(f->name, "decode"))
		return "the radio responds to selective-call addressed to it on this channel";
	if (!strcmp(f->name, "encode"))
		return "the radio sends its selective-call identity when transmitting";
	if (!strcmp(f->name, "tx_inhibit"))
		return "transmit is blocked while the channel is busy";
	if (!strcmp(f->name, "auto_ack"))
		return "acknowledge a received call automatically";
	if (!strcmp(f->name, "power_high"))
		return "RF power level: set = high";
	if (!strcmp(f->name, "reserved_b7"))
		return "a stored bit the original editor never exposes; preserved as found";
	return "";
}

static void edit_channel(int slot0)
{
	unsigned p = mc_band_p(mc_band_index(&img));
	const mc_flag *flags;
	size_t nf;
	int sel = 0, nrows;

	flags = mc_flags(img.model, &nf);
	nrows = 2 + (int)nf;

	for (;;) {
		mc_channel c;
		char buf[64], line[256];
		int i, ch;

		mc_channel_get(&img, slot0, p, &c);
		erase();
		attron(A_BOLD);
		mvprintw(0, 0, "channel %d of %s", slot0 + 1, path);
		attroff(A_BOLD);
		mvprintw(1, 0, "model %s   band %d   %u Hz steps   %s", img.model->name,
		         mc_band_index(&img), mc_step_hz(&img),
		         c.state == MC_CH_STALE ? "STALE -- past the table terminator (K-23)"
		                                : state_word(c.state));

		for (i = 0; i < nrows; i++) {
			int y = 3 + i;
			if (i == sel)
				attron(A_REVERSE);
			if (i == 0 || i == 1) {
				uint32_t hz = i == 0 ? c.tx_hz : c.rx_hz;
				if (!p || c.state == MC_CH_EMPTY || c.state == MC_CH_STALE)
					snprintf(buf, sizeof buf, "--");
				else
					fmt_mhz(buf, sizeof buf, hz);
				mvprintw(y, 2, "%-14s %-14s", i == 0 ? "TX frequency" : "RX frequency", buf);
			} else {
				const mc_flag *f = &flags[i - 2];
				mvprintw(y, 2, "%-14s %-14s", f->name,
				         mc_flag_get(&img, slot0, f) ? "yes" : "no");
			}
			if (i == sel)
				attroff(A_REVERSE);
		}

		/* the help line for whatever is selected */
		if (sel == 0)
			snprintf(line, sizeof line, "transmit frequency; stored as 3 bytes, %u Hz raster",
			         mc_step_hz(&img));
		else if (sel == 1)
			snprintf(line, sizeof line,
			         "receive frequency as displayed; the codeplug stores it minus the 21.4 MHz "
			         "first IF");
		else {
			const mc_flag *f = &flags[sel - 2];
			snprintf(line, sizeof line, "%s  [bit %d of the %s half%s%s]", flag_help(f), f->bit,
			         f->half == MC_HALF_BOTH ? "TX and RX" : f->half == MC_HALF_TX ? "TX" : "RX",
			         f->inverted ? ", stored inverted" : "",
			         f->provenance == 'S' ? ", meaning UNVERIFIED" : "");
		}
		mvprintw(3 + nrows + 1, 2, "%.*s", COLS - 4, line);
		status("up/down or j/k move   enter change   esc back");
		refresh();

		ch = getch();
		if (ch == 27 || ch == 'q')
			return;
		if ((ch == KEY_UP || ch == 'k') && sel > 0)
			sel--;
		else if ((ch == KEY_DOWN || ch == 'j') && sel < nrows - 1)
			sel++;
		else if (ch == '\n' || ch == KEY_ENTER || ch == ' ') {
			if (sel >= 2) {
				const mc_flag *f = &flags[sel - 2];
				mc_flag_set(&img, slot0, f, !mc_flag_get(&img, slot0, f));
				dirty = 1;
			} else {
				uint32_t hz;
				if (!p) {
					status("band is unprogrammed -- set the band before entering frequencies");
					getch();
					continue;
				}
				if (prompt(sel == 0 ? "TX MHz: " : "RX MHz: ", buf, sizeof buf) != 0 || !buf[0])
					continue;
				if (parse_mhz(buf, &hz) != 0) {
					status("not a frequency -- expected something like 145.17500");
					getch();
					continue;
				}
				if (mc_channel_set_freq(&img, slot0, sel == 0 ? MC_TX : MC_RX, hz) != 0) {
					/* U-3: refuse, never clamp, and say why. */
					snprintf(line, sizeof line,
					         "%s is not representable on band %d (P=%u, %u Hz steps) -- refused",
					         buf, mc_band_index(&img), p, mc_step_hz(&img));
					status(line);
					getch();
					continue;
				}
				dirty = 1;
			}
		}
	}
}

/* ---- save ----------------------------------------------------------------------------------- */

static void save_file(void)
{
	FILE *f;
	char msg[256];
	uint8_t before;

	/* An image read from a radio has no filename yet. */
	if (!path[0]) {
		char buf[480];
		if (prompt("save as: ", buf, sizeof buf) != 0 || !buf[0]) {
			status("not saved");
			getch();
			return;
		}
		snprintf(path, sizeof path, "%s", buf);
	}
	before = mc_checksum_stored(&img);
	mc_checksum_fix(&img);
	f = fopen(path, "wb");
	if (!f) {
		snprintf(msg, sizeof msg, "cannot write %.200s", path);
		status(msg);
		getch();
		return;
	}
	if (fwrite(img.bytes, 1, img.len, f) != img.len) {
		fclose(f);
		status("short write -- the file may be damaged");
		getch();
		return;
	}
	fclose(f);
	{
		size_t i, n = 0;
		for (i = 0; i < img.len; i++)
			if (orig_bytes[i] != img.bytes[i])
				n++;
		snprintf(msg, sizeof msg, "saved %.180s -- %u byte%s changed (checksum %02X -> %02X)",
		         path, (unsigned)n, n == 1 ? "" : "s", before, mc_checksum_stored(&img));
	}
	memcpy(orig_bytes, img.bytes, img.len);
	dirty = 0;
	status(msg);
	getch();
}

/* ---- PL / CTCSS, U-2 ------------------------------------------------------------------------
 * Radio-wide on every model that has it, which is why this is its own page rather than a channel
 * field: the EVA and EZA 9 hold one tone (or a list the operator picks from at the radio), not a
 * tone per channel.
 */
static void edit_pl(void)
{
	int sel = 0;

	if (!mc_pl_supported(img.model)) {
		status("this model has no PL/CTCSS that the original software can set");
		getch();
		return;
	}
	for (;;) {
		mc_pl_mode mode = mc_pl_get_mode(&img);
		int n = mc_pl_get_count(&img), rows, i, ch;
		char buf[64], line[256];

		/* MC_PL_TABLE (MCEZ13) has no mode byte: an encoder list and a decoder list, always
		 * present, so there is no mode row to cycle and twice as many tone rows. */
		if (mode == MC_PL_TABLE)
			rows = 2 * n;
		else
			rows = 1 + (mode == MC_PL_SINGLE ? 1 : mode == MC_PL_SELECTABLE ? 1 + n : 0);
		erase();
		attron(A_BOLD);
		mvprintw(0, 0, "PL / CTCSS -- %s", path[0] ? path : "(read from radio)");
		attroff(A_BOLD);
		mvprintw(1, 0, "model %s   %s   shared by the whole radio", img.model->name,
		         mc_pl_has_decoder(img.model)
		             ? "encode round(7.984 x f), decode round(61.107 x f)"
		             : "stored as round(7.984 x f)");

		for (i = 0; i < rows; i++) {
			int y = 3 + i;
			if (i == sel)
				attron(A_REVERSE);
			if (mode == MC_PL_TABLE) {
				int enc = i < n;
				int k = enc ? i : i - n;
				unsigned t = enc ? mc_pl_get_tone(&img, k) : mc_pl_dec_get(&img, k);
				snprintf(buf, sizeof buf, "%u.%u Hz", t / 10, t % 10);
				mvprintw(y, 2, "%-7s %-8d %-14s", enc ? "encode" : "decode", k + 1,
				         t ? buf : "none");
			} else if (i == 0) {
				mvprintw(y, 2, "%-16s %-14s", "mode",
				         mode == MC_PL_SINGLE ? "single tone" :
				         mode == MC_PL_SELECTABLE ? "selectable list" : "off");
			} else if (mode == MC_PL_SINGLE) {
				unsigned t = mc_pl_get_tone(&img, 0);
				snprintf(buf, sizeof buf, "%u.%u Hz", t / 10, t % 10);
				mvprintw(y, 2, "%-16s %-14s", "tone", t ? buf : "none");
			} else if (i == 1) {
				snprintf(buf, sizeof buf, "%d", n);
				mvprintw(y, 2, "%-16s %-14s", "how many tones", buf);
			} else {
				unsigned t = mc_pl_get_tone(&img, i - 2);
				snprintf(buf, sizeof buf, "%u.%u Hz", t / 10, t % 10);
				mvprintw(y, 2, "tone %-11d %-14s", i - 1, t ? buf : "none");
			}
			if (i == sel)
				attroff(A_REVERSE);
		}
		if (mode == MC_PL_TABLE)
			snprintf(line, sizeof line,
			         "decodes PL as well as encoding it (decoder law round(61.107 x f)). The "
			         "1987 software sets all ten alike; per-slot use is untested on a radio.");
		else if (sel == 0)
			snprintf(line, sizeof line,
			         "enter cycles off / single / selectable; selectable lets the operator "
			         "choose at the radio");
		else
			snprintf(line, sizeof line,
			         "%u standard tones, 67.0 to 250.3 Hz; 0 disables. Anything else is refused, "
			         "never rounded", (unsigned)(mc_pl_standard_count() - 1));
		mvprintw(3 + rows + 1, 2, "%.*s", COLS - 4, line);
		status("up/down or j/k move   enter change   esc back");
		refresh();

		ch = getch();
		if (ch == 27 || ch == 'q')
			return;
		if ((ch == KEY_UP || ch == 'k') && sel > 0)
			sel--;
		else if ((ch == KEY_DOWN || ch == 'j') && sel < rows - 1)
			sel++;
		else if (ch == '\n' || ch == KEY_ENTER || ch == ' ') {
			if (mode == MC_PL_TABLE) {
				int enc = sel < n, k = enc ? sel : sel - n;
				unsigned hz = 0, frac = 0;
				char *dot;
				if (prompt("tone in Hz (0 disables): ", buf, sizeof buf) != 0 || !buf[0])
					continue;
				dot = strchr(buf, '.');
				hz = (unsigned)atoi(buf);
				frac = dot && isdigit((unsigned char)dot[1]) ? (unsigned)(dot[1] - '0') : 0;
				if ((enc ? mc_pl_set_tone(&img, k, hz * 10 + frac)
				         : mc_pl_dec_set(&img, k, hz * 10 + frac)) != 0) {
					snprintf(line, sizeof line,
					         "%s is outside 67.0-250.3 Hz -- refused, not rounded", buf);
					status(line);
					getch();
					continue;
				}
				dirty = 1;
			} else if (sel == 0) {
				mc_pl_set_mode(&img, mode == MC_PL_OFF ? MC_PL_SINGLE :
				                     mode == MC_PL_SINGLE ? MC_PL_SELECTABLE : MC_PL_OFF);
				if (mc_pl_get_mode(&img) == MC_PL_SELECTABLE && mc_pl_get_count(&img) < 1)
					mc_pl_set_count(&img, 1);
				dirty = 1;
				sel = 0;
			} else if (mode == MC_PL_SELECTABLE && sel == 1) {
				if (prompt("how many tones (1-10): ", buf, sizeof buf) == 0 && buf[0]) {
					mc_pl_set_count(&img, atoi(buf));
					dirty = 1;
				}
			} else {
				int idx = mode == MC_PL_SINGLE ? 0 : sel - 2;
				unsigned hz = 0, frac = 0;
				if (prompt("tone in Hz (0 disables): ", buf, sizeof buf) != 0 || !buf[0])
					continue;
				{
					char *dot = strchr(buf, '.');
					hz = (unsigned)atoi(buf);
					frac = dot && isdigit((unsigned char)dot[1])
					           ? (unsigned)(dot[1] - '0') : 0;
				}
				if (mc_pl_set_tone(&img, idx, hz * 10 + frac) != 0) {
					snprintf(line, sizeof line,
					         "%s is outside 67.0-250.3 Hz -- refused, not rounded", buf);
					status(line);
					getch();
					continue;
				}
				dirty = 1;
			}
		}
	}
}

/* ---- entry point ----------------------------------------------------------------------------- */

int mc_tui_run(mc_image *image, const char *filepath, const char *note)
{
	int sel = 0, top = 0;

	img = *image;
	snprintf(path, sizeof path, "%s", filepath ? filepath : "");
	memcpy(orig_bytes, img.bytes, img.len);
	dirty = 0;

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	opening_note = (note && *note) ? note : NULL;

	for (;;) {
		int ch, rows = LINES - 5;
		draw_list(sel, top);
		ch = getch();
		opening_note = NULL; /* the first key dismisses the note AND does its job */
		if (ch == 'q') {
			if (dirty) {
				char buf[8];
				if (prompt("unsaved changes -- quit anyway? [y/N] ", buf, sizeof buf) == 0 &&
				    (buf[0] == 'y' || buf[0] == 'Y'))
					break;
				continue;
			}
			break;
		} else if ((ch == KEY_UP || ch == 'k') && sel > 0)
			sel--;
		else if ((ch == KEY_DOWN || ch == 'j') && sel < img.model->nchan - 1)
			sel++;
		else if (ch == '\n' || ch == KEY_ENTER)
			edit_channel(sel);
		else if (ch == 'p')
			edit_pl();
		else if (ch == 's')
			save_file();
		if (sel < top)
			top = sel;
		if (sel >= top + rows)
			top = sel - rows + 1;
	}
	endwin();
	return 0;
}
