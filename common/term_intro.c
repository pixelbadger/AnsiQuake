/*
Copyright (C) 2026 AnsiQuake contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/
// term_intro.c -- demoscene-style ANSI text intro played once at startup.
//
// Draws an animated fire-plasma field (rendered with the same truecolor
// half-block technique the game itself uses) with a block-art "ANSIQUAKE"
// logo stamped over it and a real-text caption underneath, so the very
// first thing the player sees makes it obvious the whole engine draws with
// terminal characters. Runs entirely on TTY_Write/TTY_GetSize/TTY_ReadInput
// -- no Quake framebuffer or palette required.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sys.h"
#include "term_intro.h"
#include "term_tty.h"

#define TERM_INTRO_DURATION   3.0
#define TERM_INTRO_FRAME_US   33000
#define TERM_INTRO_SYNC_BEGIN "\033[?2026h"
#define TERM_INTRO_SYNC_END   "\033[?2026l"

typedef struct {
    char ch;
    byte rows[7];
} term_intro_glyph_t;

/* Minimal 5x7 block font -- only the glyphs needed to spell "ANSIQUAKE". */
static const term_intro_glyph_t term_intro_font[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x19, 0x15, 0x15, 0x13, 0x11, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'I', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
};

static const char term_intro_word[] = "ANSIQUAKE";

static const byte *
TERM_IntroFindGlyph(char ch)
{
    size_t i;

    for (i = 0; i < sizeof(term_intro_font) / sizeof(term_intro_font[0]); i++)
	if (term_intro_font[i].ch == ch)
	    return term_intro_font[i].rows;
    return NULL;
}

static inline void
TERM_IntroSetPixel(byte *pixels, int cols, int ph, int x, int y, int r, int g, int b)
{
    byte *p;

    if (x < 0 || x >= cols || y < 0 || y >= ph)
	return;
    p = pixels + (size_t)(y * cols + x) * 3;
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

static void
TERM_IntroFillRect(byte *pixels, int cols, int ph, int x0, int y0, int w, int h,
		    int r, int g, int b)
{
    int x, y;

    for (y = y0; y < y0 + h; y++)
	for (x = x0; x < x0 + w; x++)
	    TERM_IntroSetPixel(pixels, cols, ph, x, y, r, g, b);
}

static void
TERM_IntroFireColor(float v, byte *out)
{
    static const byte stops[5][3] = {
	{ 0, 0, 0 }, { 90, 12, 6 }, { 200, 60, 10 }, { 240, 150, 20 }, { 255, 235, 140 },
    };
    float f = v * 4.0f;
    int i = (int)f;
    float frac;

    if (i < 0) i = 0;
    if (i > 3) i = 3;
    frac = f - i;
    out[0] = (byte)(stops[i][0] + (stops[i + 1][0] - stops[i][0]) * frac);
    out[1] = (byte)(stops[i][1] + (stops[i + 1][1] - stops[i][1]) * frac);
    out[2] = (byte)(stops[i][2] + (stops[i + 1][2] - stops[i][2]) * frac);
}

static void
TERM_IntroRenderPlasma(byte *pixels, int cols, int ph, double t)
{
    int x, y;
    float cx = cols / 2.0f, cy = ph / 2.0f;
    float ft = (float)t;

    for (y = 0; y < ph; y++) {
	for (x = 0; x < cols; x++) {
	    float dx = x - cx, dy = y - cy;
	    float v = sinf(x * 0.06f + ft * 1.6f)
		    + sinf(y * 0.10f - ft * 1.1f)
		    + sinf((x + y) * 0.035f + ft * 0.8f)
		    + sinf(sqrtf(dx * dx + dy * dy) * 0.06f - ft * 1.3f);

	    v = (v + 4.0f) / 8.0f;
	    if (v < 0.0f) v = 0.0f;
	    if (v > 1.0f) v = 1.0f;
	    TERM_IntroFireColor(v, pixels + (size_t)(y * cols + x) * 3);
	}
    }
}

static void
TERM_IntroStampLogo(byte *pixels, int cols, int ph, int x0, int y0, int scale)
{
    int letter_w = 5 * scale, letter_h = 7 * scale, gap = scale;
    int word_len = (int)(sizeof(term_intro_word) - 1);
    int word_w = word_len * (letter_w + gap) - gap;
    int li, gx, gy, px, py;

    TERM_IntroFillRect(pixels, cols, ph, x0 - 2 * scale, y0 - 2 * scale,
			word_w + 4 * scale, letter_h + 4 * scale, 10, 10, 14);

    for (li = 0; term_intro_word[li]; li++) {
	const byte *rows = TERM_IntroFindGlyph(term_intro_word[li]);
	int lx0 = x0 + li * (letter_w + gap);

	if (!rows)
	    continue;
	for (gy = 0; gy < 7; gy++) {
	    for (gx = 0; gx < 5; gx++) {
		if (!(rows[gy] & (1 << (4 - gx))))
		    continue;
		for (py = 0; py < scale; py++)
		    for (px = 0; px < scale; px++)
			TERM_IntroSetPixel(pixels, cols, ph, lx0 + gx * scale + px,
					   y0 + gy * scale + py, 255, 190, 60);
	    }
	}
    }
}

static void
TERM_IntroDrawCaption(int cols, int rows)
{
    static const char line1[] = "RENDERED ENTIRELY IN ANSI TRUECOLOR HALF-BLOCK CHARACTERS";
    static const char line2[] = "PRESS ANY KEY TO CONTINUE";
    char buf[256];
    int len, col0;

    len = (int)(sizeof(line1) - 1);
    col0 = (cols - len) / 2;
    if (col0 < 1) col0 = 1;
    snprintf(buf, sizeof(buf), "\033[%d;%dH\033[38;2;210;210;210;48;2;0;0;0m%s",
	     rows - 1, col0, line1);
    TTY_Write(buf, strlen(buf));

    len = (int)(sizeof(line2) - 1);
    col0 = (cols - len) / 2;
    if (col0 < 1) col0 = 1;
    snprintf(buf, sizeof(buf), "\033[%d;%dH\033[38;2;120;120;120;48;2;0;0;0m%s",
	     rows, col0, line2);
    TTY_Write(buf, strlen(buf));
}

static char *
TERM_IntroPutNum(char *p, int num)
{
    char tmp[12];
    int len = 0;

    do {
	tmp[len++] = '0' + num % 10;
	num /= 10;
    } while (num);
    while (len)
	*p++ = tmp[--len];

    return p;
}

static void
TERM_IntroEmitFrame(char *outbuf, const byte *pixels, int cols, int usable_rows)
{
    char *p = outbuf;
    int r, c, cur_row = -1, cur_col = -1;
    int cur_fr = -1, cur_fg = -1, cur_fb = -1;
    int cur_br = -1, cur_bg = -1, cur_bb = -1;

    memcpy(p, TERM_INTRO_SYNC_BEGIN, sizeof(TERM_INTRO_SYNC_BEGIN) - 1);
    p += sizeof(TERM_INTRO_SYNC_BEGIN) - 1;

    for (r = 0; r < usable_rows; r++) {
	const byte *top = pixels + (size_t)(r * 2) * cols * 3;
	const byte *bot = pixels + (size_t)(r * 2 + 1) * cols * 3;

	for (c = 0; c < cols; c++) {
	    const byte *tp = top + c * 3;
	    const byte *bp = bot + c * 3;

	    if (r != cur_row || c != cur_col) {
		*p++ = '\033';
		*p++ = '[';
		p = TERM_IntroPutNum(p, r + 1);
		*p++ = ';';
		p = TERM_IntroPutNum(p, c + 1);
		*p++ = 'H';
		cur_row = r;
		cur_col = c;
	    }

	    if (tp[0] == bp[0] && tp[1] == bp[1] && tp[2] == bp[2]) {
		if (cur_br != bp[0] || cur_bg != bp[1] || cur_bb != bp[2]) {
		    p += sprintf(p, "\033[48;2;%d;%d;%dm", bp[0], bp[1], bp[2]);
		    cur_br = bp[0];
		    cur_bg = bp[1];
		    cur_bb = bp[2];
		}
		*p++ = ' ';
	    } else {
		if (cur_fr != tp[0] || cur_fg != tp[1] || cur_fb != tp[2] ||
		    cur_br != bp[0] || cur_bg != bp[1] || cur_bb != bp[2]) {
		    p += sprintf(p, "\033[38;2;%d;%d;%d;48;2;%d;%d;%dm",
				 tp[0], tp[1], tp[2], bp[0], bp[1], bp[2]);
		    cur_fr = tp[0];
		    cur_fg = tp[1];
		    cur_fb = tp[2];
		    cur_br = bp[0];
		    cur_bg = bp[1];
		    cur_bb = bp[2];
		}
		*p++ = '\xe2';
		*p++ = '\x96';
		*p++ = '\x80';
	    }
	    cur_col++;
	}
    }

    memcpy(p, TERM_INTRO_SYNC_END, sizeof(TERM_INTRO_SYNC_END) - 1);
    p += sizeof(TERM_INTRO_SYNC_END) - 1;

    TTY_Write(outbuf, p - outbuf);
}

byte *
TERM_PlayIntro(int *out_cols, int *out_rows)
{
    int cols, rows, usable_rows, ph, scale, word_len, logo_w, logo_h, x0, y0;
    byte *pixels;
    char *outbuf;
    double t0, elapsed;

    TTY_GetSize(&cols, &rows);
    if (cols < 20 || rows < 10)
	return NULL;

    usable_rows = rows - 2;
    ph = rows * 2;

    pixels = malloc((size_t)cols * ph * 3);
    outbuf = malloc((size_t)cols * rows * 64 + 256);
    if (!pixels || !outbuf) {
	free(pixels);
	free(outbuf);
	return NULL;
    }

    word_len = (int)(sizeof(term_intro_word) - 1);
    scale = (cols * 7 / 10) / (word_len * 6 - 1);
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    logo_w = word_len * (5 * scale + scale) - scale;
    logo_h = 7 * scale;
    x0 = (cols - logo_w) / 2;
    if (x0 < 0) x0 = 0;
    y0 = (usable_rows * 2 - logo_h) / 2;
    if (y0 < 0) y0 = 0;

    TERM_IntroDrawCaption(cols, rows);

    t0 = Sys_DoubleTime();
    for (;;) {
	char skipbuf[64];

	elapsed = Sys_DoubleTime() - t0;
	TERM_IntroRenderPlasma(pixels, cols, usable_rows * 2, elapsed);
	TERM_IntroStampLogo(pixels, cols, usable_rows * 2, x0, y0, scale);
	TERM_IntroEmitFrame(outbuf, pixels, cols, usable_rows);

	if (elapsed >= TERM_INTRO_DURATION)
	    break;
	if (TTY_ReadInput(skipbuf, sizeof(skipbuf)) > 0)
	    break;
	usleep(TERM_INTRO_FRAME_US);
    }

    {
	int x, y;

	for (y = usable_rows * 2; y < ph; y++)
	    for (x = 0; x < cols; x++) {
		byte *p = pixels + (size_t)(y * cols + x) * 3;
		p[0] = p[1] = p[2] = 0;
	    }
    }

    free(outbuf);
    *out_cols = cols;
    *out_rows = rows;
    return pixels;
}
