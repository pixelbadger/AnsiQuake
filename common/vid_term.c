/*
Copyright (C) 1996-1997 Id Software, Inc.
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
// vid_term.c -- ANSI terminal video driver using truecolor half-blocks
//
// Each character cell shows two pixels: U+2580 UPPER HALF BLOCK with the
// foreground color carrying the top pixel and the background color the
// bottom pixel.  Only cells that changed since the previous frame are
// re-emitted, wrapped in synchronized-update guards to avoid tearing.

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cmd.h"
#include "common.h"
#include "console.h"
#include "cvar.h"
#include "d_iface.h"
#include "d_local.h"
#include "draw.h"
#include "input.h"
#include "keys.h"
#include "menu.h"
#include "quakedef.h"
#include "screen.h"
#include "sys.h"
#include "term_intro.h"
#include "term_tty.h"
#include "vid.h"
#include "view.h"
#include "wad.h"

#ifdef NQ_HACK
#include "host.h"
#endif
#ifdef QW_HACK
#include "client.h"
#endif

viddef_t vid;			/* global video state */
unsigned short d_8to16table[256];
unsigned d_8to24table[256];

static byte *vid_surfcache;
static int vid_surfcachesize;
static int VID_highhunkmark;

static qboolean palette_changed;

/* Terminal cell grid state */
static int term_cols, term_rows;
static uint16_t *term_cells;	/* previous frame, (top << 8) | bottom per cell */
static int *term_xmap;		/* grid column -> render buffer x */
static int *term_ymap;		/* grid half-row -> render buffer y */
static char *term_outbuf;
static qboolean term_dirty_all;
static qboolean term_skipped_last;
static double term_write_time;	/* seconds spent writing the previous frame */

/* Startup intro -> game melt transition (see TERM_DoWipe) */
static byte *term_wipe_src;	/* RGB888, term_wipe_w x term_wipe_h*2 */
static int term_wipe_w, term_wipe_h;
static qboolean term_wipe_pending;

/* Precomputed SGR fragments per palette index: "38;2;R;G;B" / "48;2;R;G;B" */
static char term_fg_sgr[256][20];
static char term_bg_sgr[256][20];
static byte term_fg_len[256];
static byte term_bg_len[256];

#define TERM_FRAME_BUDGET (1.0 / 72.0)
#define TERM_SYNC_BEGIN "\033[?2026h"
#define TERM_SYNC_END   "\033[?2026l"

/*
 * Per-frame benchmark stats, enabled with QSTATS=<file> in the environment
 * (analyzed by scripts/benchstats.py).  One CSV record per VID_Update call.
 * Records go out through a raw write() per frame so they survive the
 * SIGTERM/fatal-signal exit path, which never flushes stdio.
 */
static int term_stats_fd = -2;	/* -2 = QSTATS not checked yet, -1 = off */
static int term_stats_frame;
static double term_stats_epoch;

static void
TERM_StatsOpen(void)
{
    const char *path = getenv("QSTATS");
    char header[128];
    int len;

    term_stats_fd = -1;
    if (!path || !*path)
	return;
    term_stats_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (term_stats_fd < 0)
	return;
    len = snprintf(header, sizeof(header),
		   "# cols=%d rows=%d render=%dx%d\n"
		   "frame,ms,changed,bytes,gen_us,write_us,skipped\n",
		   term_cols, term_rows, vid.width, vid.height);
    write(term_stats_fd, header, len);
    term_stats_epoch = Sys_DoubleTime();
}

static void
TERM_StatsRecord(int changed, int bytes, double gen, double wr, int skipped)
{
    char line[96];
    int len;

    if (term_stats_fd < 0)
	return;
    len = snprintf(line, sizeof(line), "%d,%.1f,%d,%d,%d,%d,%d\n",
		   term_stats_frame++,
		   (Sys_DoubleTime() - term_stats_epoch) * 1000.0,
		   changed, bytes,
		   (int)(gen * 1e6), (int)(wr * 1e6), skipped);
    write(term_stats_fd, line, len);
}

void
VID_GetDesktopRect(vrect_t *rect)
{
    rect->x = 0;
    rect->y = 0;
    rect->width = vid.width;
    rect->height = vid.height;
}

void
VID_Shutdown(void)
{
    TTY_Shutdown();
}

void
VID_ShiftPalette(const byte *palette)
{
    VID_SetPalette(palette);
}

void
VID_SetDefaultMode(void)
{
}

qboolean
window_visible(void)
{
    return true;
}

void
VID_SetPalette(const byte *palette)
{
    unsigned i, r, g, b;

    for (i = 0; i < 256; i++) {
	r = palette[0];
	g = palette[1];
	b = palette[2];
	palette += 3;
	d_8to24table[i] = (r << 16) | (g << 8) | b;
	term_fg_len[i] = snprintf(term_fg_sgr[i], sizeof(term_fg_sgr[i]),
				  "38;2;%u;%u;%u", r, g, b);
	term_bg_len[i] = snprintf(term_bg_sgr[i], sizeof(term_bg_sgr[i]),
				  "48;2;%u;%u;%u", r, g, b);
    }
    palette_changed = true;
}

void
VID_InitColormap(const byte *palette)
{
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
}

qboolean
VID_CheckAdequateMem(int width, int height)
{
    int tbuffersize;

    tbuffersize = width * height * sizeof(*d_pzbuffer);
    tbuffersize += D_SurfaceCacheForRes(width, height);

    /*
     * see if there's enough memory, allowing for the normal mode 0x13 pixel,
     * z, and surface buffers
     */
    if ((host_parms.memsize - tbuffersize + SURFCACHE_SIZE_AT_320X200 +
	 0x10000 * 3) < minimum_memory)
	return false;

    return true;
}

static qboolean
VID_AllocBuffers(int width, int height)
{
    int tsize, tbuffersize;

    tsize = D_SurfaceCacheForRes(width, height);
    tbuffersize = width * height * sizeof(*d_pzbuffer);
    tbuffersize += tsize;

    /*
     * see if there's enough memory, allowing for the normal mode 0x13 pixel,
     * z, and surface buffers
     */
    if ((host_parms.memsize - tbuffersize + SURFCACHE_SIZE_AT_320X200 + 0x10000 * 3) < minimum_memory) {
	Con_SafePrintf("Not enough memory for video mode\n");
	return false;
    }

    vid_surfcachesize = tsize;

    if (d_pzbuffer) {
	D_FlushCaches();
	Hunk_FreeToHighMark(VID_highhunkmark);
	d_pzbuffer = NULL;
    }

    VID_highhunkmark = Hunk_HighMark();
    d_pzbuffer = Hunk_HighAllocName(tbuffersize, "video");
    vid_surfcache = (byte *)d_pzbuffer + width * height * sizeof(*d_pzbuffer);
    r_warpbuffer = Hunk_HighAllocName(width * height, "warpbuf");

    vid.buffer = vid.conbuffer = vid.direct = Hunk_HighAllocName(width * height, "vidbuf");
    vid.rowbytes = vid.conrowbytes = width;

    R_AllocSurfEdges(false);

    return true;
}

/*
 * The terminal dictates the video mode: one grid pixel per half character
 * cell.  The software renderer needs at least MINWIDTH x MINHEIGHT and a
 * width that is a multiple of 8, so on small terminals we render at the
 * minimum size and sample down to the cell grid via the x/y index maps.
 */
static void
TERM_SetupGrid(void)
{
    int i, gridw, gridh, renderw, renderh;

    TTY_GetSize(&term_cols, &term_rows);
    gridw = term_cols;
    gridh = term_rows * 2;

    renderw = qclamp(gridw & ~7, MINWIDTH, MAXWIDTH & ~7);
    renderh = qclamp(gridh, MINHEIGHT, MAXHEIGHT);

    vid.width = vid.conwidth = renderw;
    vid.height = vid.conheight = renderh;
    vid.output.width = renderw;
    vid.output.height = renderh;
    vid.output.scale = 1;
    vid.aspect = 1;
    vid.numpages = 1;

    free(term_cells);
    free(term_xmap);
    free(term_ymap);
    free(term_outbuf);
    term_cells = malloc(term_cols * term_rows * sizeof(*term_cells));
    term_xmap = malloc(term_cols * sizeof(*term_xmap));
    term_ymap = malloc(gridh * sizeof(*term_ymap));
    /* worst case per cell: cursor move + full SGR + 3-byte glyph */
    term_outbuf = malloc((size_t)term_cols * term_rows * 64 + 256);
    if (!term_cells || !term_xmap || !term_ymap || !term_outbuf)
	Sys_Error("%s: out of memory", __func__);

    for (i = 0; i < term_cols; i++)
	term_xmap[i] = i * renderw / gridw;
    for (i = 0; i < gridh; i++)
	term_ymap[i] = i * renderh / gridh;

    term_dirty_all = true;
}

qboolean
VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
    TERM_SetupGrid();

    VID_SetPalette(palette);
    VID_InitColormap(palette);

    if (!VID_AllocBuffers(vid.width, vid.height))
	return false;

    D_InitCaches(vid_surfcache, vid_surfcachesize);

    /* Keep the mode framework informed for anything that peeks at it */
    vid_windowed_mode.width = vid.width;
    vid_windowed_mode.height = vid.height;
    vid_windowed_mode.bpp = 24;
    vid_windowed_mode.refresh = 72;
    vid_windowed_mode.resolution.scale = 1;
    vid_windowed_mode.resolution.width = vid.width;
    vid_windowed_mode.resolution.height = vid.height;
    vid_currentmode = &vid_windowed_mode;

    vid.recalc_refdef = 1;

    SCR_CheckResize();
    Con_CheckResize();

    return true;
}

void
VID_Init(const byte *palette)
{
    TTY_Init();
    term_wipe_src = TERM_PlayIntro(&term_wipe_w, &term_wipe_h);

    VID_SetMode(&vid_windowed_mode, palette);
    term_wipe_pending = term_wipe_src != NULL;

    /* Resolution is dictated by the terminal; no video menu */
    vid_menudrawfn = NULL;
    vid_menukeyfn = NULL;
}

void
VID_RegisterVariables(void)
{
}

void
VID_AddCommands(void)
{
}

static inline char *
TERM_PutNum(char *p, int num)
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

/*
 * Classic Doom "melt": each column drips from the outgoing (intro) image
 * to the incoming (first real game) image at its own randomized rate.
 * Runs synchronously for its ~1s duration -- there is nothing else for the
 * engine to be doing at this point in startup.
 */
static inline unsigned
TERM_WipeSrcPixel(int col, int y)
{
    const byte *p = term_wipe_src + (size_t)(y * term_wipe_w + col) * 3;

    return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
}

static void
TERM_DoWipe(void)
{
    int width = term_wipe_w;
    int height = term_wipe_h * 2;
    int *dy;
    int i, r, c, done;

    dy = malloc(width * sizeof(*dy));
    if (!dy)
	return;

    dy[0] = -(rand() % 16);
    for (i = 1; i < width; i++) {
	int step = (rand() % 3) - 1;
	dy[i] = dy[i - 1] + step;
	if (dy[i] > 0) dy[i] = 0;
	if (dy[i] < -15) dy[i] = -15;
    }

    do {
	char *p = term_outbuf;
	int cur_fr = -1, cur_fg = -1, cur_fb = -1;
	int cur_br = -1, cur_bg = -1, cur_bb = -1;
	int cur_row = -1, cur_col = -1;

	done = 1;
	for (i = 0; i < width; i++) {
	    if (dy[i] < 0) {
		dy[i]++;
		done = 0;
		continue;
	    }
	    if (dy[i] < height) {
		int step = (dy[i] < 16) ? dy[i] + 1 : 8;
		if (dy[i] + step > height)
		    step = height - dy[i];
		dy[i] += step;
		done = 0;
	    }
	}

	memcpy(p, TERM_SYNC_BEGIN, sizeof(TERM_SYNC_BEGIN) - 1);
	p += sizeof(TERM_SYNC_BEGIN) - 1;

	for (r = 0; r < term_wipe_h; r++) {
	    const byte *new_top = vid.buffer + term_ymap[r * 2] * vid.rowbytes;
	    const byte *new_bot = vid.buffer + term_ymap[r * 2 + 1] * vid.rowbytes;

	    for (c = 0; c < width; c++) {
		/* Columns still in their randomized holdback (dy < 0) show
		   the old frame completely undisturbed. */
		int edy = (dy[c] < 0) ? 0 : dy[c];
		int py0 = r * 2, py1 = r * 2 + 1;
		int oy0 = py0 - edy, oy1 = py1 - edy;
		unsigned top_rgb, bot_rgb;
		int tr, tg, tb, br, bg, bb;

		top_rgb = (py0 < edy || oy0 >= height)
		    ? d_8to24table[new_top[term_xmap[c]]]
		    : TERM_WipeSrcPixel(c, oy0);
		bot_rgb = (py1 < edy || oy1 >= height)
		    ? d_8to24table[new_bot[term_xmap[c]]]
		    : TERM_WipeSrcPixel(c, oy1);

		if (r != cur_row || c != cur_col) {
		    *p++ = '\033';
		    *p++ = '[';
		    p = TERM_PutNum(p, r + 1);
		    *p++ = ';';
		    p = TERM_PutNum(p, c + 1);
		    *p++ = 'H';
		    cur_row = r;
		    cur_col = c;
		}

		tr = (top_rgb >> 16) & 0xff;
		tg = (top_rgb >> 8) & 0xff;
		tb = top_rgb & 0xff;
		br = (bot_rgb >> 16) & 0xff;
		bg = (bot_rgb >> 8) & 0xff;
		bb = bot_rgb & 0xff;

		if (tr == br && tg == bg && tb == bb) {
		    if (cur_br != br || cur_bg != bg || cur_bb != bb) {
			p += sprintf(p, "\033[48;2;%d;%d;%dm", br, bg, bb);
			cur_br = br;
			cur_bg = bg;
			cur_bb = bb;
		    }
		    *p++ = ' ';
		} else {
		    if (cur_fr != tr || cur_fg != tg || cur_fb != tb ||
			cur_br != br || cur_bg != bg || cur_bb != bb) {
			p += sprintf(p, "\033[38;2;%d;%d;%d;48;2;%d;%d;%dm",
				     tr, tg, tb, br, bg, bb);
			cur_fr = tr;
			cur_fg = tg;
			cur_fb = tb;
			cur_br = br;
			cur_bg = bg;
			cur_bb = bb;
		    }
		    *p++ = '\xe2';
		    *p++ = '\x96';
		    *p++ = '\x80';
		}
		cur_col++;
	    }
	}

	memcpy(p, TERM_SYNC_END, sizeof(TERM_SYNC_END) - 1);
	p += sizeof(TERM_SYNC_END) - 1;

	TTY_Write(term_outbuf, p - term_outbuf);
	usleep(1000000 / 35);
    } while (!done);

    free(dy);
}

void
VID_Update(vrect_t *rects)
{
    int r, c, cur_fg, cur_bg, cur_row, cur_col;
    int changed;
    uint16_t *cell;
    char *p;
    double start, gen_start;
    qboolean any;

    if (term_stats_fd == -2)
	TERM_StatsOpen();

    if (term_wipe_pending) {
	if (term_wipe_w == term_cols && term_wipe_h == term_rows)
	    TERM_DoWipe();
	free(term_wipe_src);
	term_wipe_src = NULL;
	term_wipe_pending = false;
	term_dirty_all = true;
	return;
    }

    if (TTY_CheckResume())
	term_dirty_all = true;
    if (TTY_CheckResize()) {
	VID_SetMode(&vid_windowed_mode, host_basepal);
	return;		/* buffer was reallocated; draw next frame */
    }

    if (palette_changed) {
	palette_changed = false;
	term_dirty_all = true;
    }

    /*
     * If the terminal can't keep pace, drop at most every other frame so
     * the tty never throttles the engine below its native speed.
     */
    if (term_write_time > TERM_FRAME_BUDGET && !term_skipped_last && !term_dirty_all) {
	term_skipped_last = true;
	term_write_time = 0;
	TERM_StatsRecord(0, 0, 0, 0, 1);
	return;
    }
    term_skipped_last = false;

    gen_start = Sys_DoubleTime();
    p = term_outbuf;
    memcpy(p, TERM_SYNC_BEGIN, sizeof(TERM_SYNC_BEGIN) - 1);
    p += sizeof(TERM_SYNC_BEGIN) - 1;

    cur_fg = cur_bg = -1;
    cur_row = cur_col = -1;
    any = false;
    changed = 0;
    cell = term_cells;

    for (r = 0; r < term_rows; r++) {
	const byte *top = vid.buffer + term_ymap[r * 2] * vid.rowbytes;
	const byte *bottom = vid.buffer + term_ymap[r * 2 + 1] * vid.rowbytes;

	for (c = 0; c < term_cols; c++, cell++) {
	    int t = top[term_xmap[c]];
	    int b = bottom[term_xmap[c]];
	    uint16_t value = (t << 8) | b;

	    if (!term_dirty_all && *cell == value)
		continue;
	    *cell = value;
	    any = true;
	    changed++;

	    if (r != cur_row || c != cur_col) {
		*p++ = '\033';
		*p++ = '[';
		p = TERM_PutNum(p, r + 1);
		*p++ = ';';
		p = TERM_PutNum(p, c + 1);
		*p++ = 'H';
		cur_row = r;
		cur_col = c;
	    }

	    if (t == b) {
		/* both pixels equal: a space needs only the background */
		if (cur_bg != t) {
		    *p++ = '\033';
		    *p++ = '[';
		    memcpy(p, term_bg_sgr[t], term_bg_len[t]);
		    p += term_bg_len[t];
		    *p++ = 'm';
		    cur_bg = t;
		}
		*p++ = ' ';
	    } else {
		if (cur_fg != t || cur_bg != b) {
		    *p++ = '\033';
		    *p++ = '[';
		    if (cur_fg != t) {
			memcpy(p, term_fg_sgr[t], term_fg_len[t]);
			p += term_fg_len[t];
			cur_fg = t;
			if (cur_bg != b)
			    *p++ = ';';
		    }
		    if (cur_bg != b) {
			memcpy(p, term_bg_sgr[b], term_bg_len[b]);
			p += term_bg_len[b];
			cur_bg = b;
		    }
		    *p++ = 'm';
		}
		/* U+2580 UPPER HALF BLOCK */
		*p++ = '\xe2';
		*p++ = '\x96';
		*p++ = '\x80';
	    }
	    cur_col++;
	}
    }
    term_dirty_all = false;

    if (!any) {
	term_write_time = 0;
	TERM_StatsRecord(0, 0, Sys_DoubleTime() - gen_start, 0, 0);
	return;
    }

    memcpy(p, TERM_SYNC_END, sizeof(TERM_SYNC_END) - 1);
    p += sizeof(TERM_SYNC_END) - 1;

    start = Sys_DoubleTime();
    TTY_Write(term_outbuf, p - term_outbuf);
    term_write_time = Sys_DoubleTime() - start;
    TERM_StatsRecord(changed, p - term_outbuf, start - gen_start,
		     term_write_time, 0);
}

void
D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
}

void
D_EndDirectRect(int x, int y, int width, int height)
{
}

void
VID_LockBuffer(void)
{
}

void
VID_UnlockBuffer(void)
{
}

void
VID_ProcessEvents(void)
{
    IN_TERM_ProcessInput();
}

#ifndef _WIN32
void
Sys_SendKeyEvents(void)
{
    VID_ProcessEvents();
}
#endif
