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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Precomputed SGR fragments per palette index: "38;2;R;G;B" / "48;2;R;G;B" */
static char term_fg_sgr[256][20];
static char term_bg_sgr[256][20];
static byte term_fg_len[256];
static byte term_bg_len[256];

#define TERM_FRAME_BUDGET (1.0 / 72.0)
#define TERM_SYNC_BEGIN "\033[?2026h"
#define TERM_SYNC_END   "\033[?2026l"

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
    VID_SetMode(&vid_windowed_mode, palette);

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

void
VID_Update(vrect_t *rects)
{
    int r, c, cur_fg, cur_bg, cur_row, cur_col;
    uint16_t *cell;
    char *p;
    double start;
    qboolean any;

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
	return;
    }
    term_skipped_last = false;

    p = term_outbuf;
    memcpy(p, TERM_SYNC_BEGIN, sizeof(TERM_SYNC_BEGIN) - 1);
    p += sizeof(TERM_SYNC_BEGIN) - 1;

    cur_fg = cur_bg = -1;
    cur_row = cur_col = -1;
    any = false;
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
	return;
    }

    memcpy(p, TERM_SYNC_END, sizeof(TERM_SYNC_END) - 1);
    p += sizeof(TERM_SYNC_END) - 1;

    start = Sys_DoubleTime();
    TTY_Write(term_outbuf, p - term_outbuf);
    term_write_time = Sys_DoubleTime() - start;
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
