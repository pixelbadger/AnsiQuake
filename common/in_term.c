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
// in_term.c -- keyboard input via the kitty keyboard protocol
//
// Plain terminals only report key presses; an FPS needs releases too.
// The kitty keyboard protocol (kitty, ghostty, wezterm, foot, recent
// alacritty) reports press/repeat/release for every key as CSI sequences.

#include <string.h>

#include "cvar.h"
#include "keys.h"
#include "quakedef.h"
#include "sys.h"
#include "term_tty.h"

#ifdef NQ_HACK
#include "client.h"
#endif
#ifdef QW_HACK
#include "protocol.h"
#endif

cvar_t _windowed_mouse = {
    .name = "_windowed_mouse",
    .string = "0",
    .flags = CVAR_CONFIG,
};

/* kitty protocol event types */
#define KITTY_EVENT_PRESS   1
#define KITTY_EVENT_REPEAT  2
#define KITTY_EVENT_RELEASE 3

#define KITTY_MOD_CTRL 4

static char in_buf[1024];
static int in_buflen;
static int in_pending_esc_frames;

static double quitpress_times[3];
static int quitpress_index;

/* Functional key codepoints from the kitty protocol spec (57344 + N) */
static knum_t
IN_TERM_MapFunctionalKey(int codepoint)
{
    switch (codepoint) {
    case 57358: return K_CAPSLOCK;
    case 57359: return K_SCROLLOCK;
    case 57360: return K_NUMLOCK;
    case 57361: return K_PRINT;
    case 57362: return K_PAUSE;
    case 57363: return K_MENU;
    case 57376: return K_F13;
    case 57377: return K_F14;
    case 57378: return K_F15;
    case 57399: return K_KP0;
    case 57400: return K_KP1;
    case 57401: return K_KP2;
    case 57402: return K_KP3;
    case 57403: return K_KP4;
    case 57404: return K_KP5;
    case 57405: return K_KP6;
    case 57406: return K_KP7;
    case 57407: return K_KP8;
    case 57408: return K_KP9;
    case 57409: return K_KP_PERIOD;
    case 57410: return K_KP_DIVIDE;
    case 57411: return K_KP_MULTIPLY;
    case 57412: return K_KP_MINUS;
    case 57413: return K_KP_PLUS;
    case 57414: return K_KP_ENTER;
    case 57415: return K_KP_EQUALS;
    case 57417: return K_LEFTARROW;
    case 57418: return K_RIGHTARROW;
    case 57419: return K_UPARROW;
    case 57420: return K_DOWNARROW;
    case 57421: return K_PGUP;
    case 57422: return K_PGDN;
    case 57423: return K_HOME;
    case 57424: return K_END;
    case 57425: return K_INS;
    case 57426: return K_DEL;
    case 57441: return K_LSHIFT;
    case 57442: return K_LCTRL;
    case 57443: return K_LALT;
    case 57444: return K_LSUPER;
    case 57445: return K_LMETA;
    case 57446: return K_LMETA;
    case 57447: return K_RSHIFT;
    case 57448: return K_RCTRL;
    case 57449: return K_RALT;
    case 57450: return K_RSUPER;
    case 57451: return K_RMETA;
    case 57452: return K_RMETA;
    default: return K_UNKNOWN;
    }
}

static knum_t
IN_TERM_MapCodepoint(int codepoint)
{
    if (codepoint >= 'A' && codepoint <= 'Z')
	return codepoint - 'A' + 'a';
    switch (codepoint) {
    case 9:   return K_TAB;
    case 13:  return K_ENTER;
    case 27:  return K_ESCAPE;
    case 127: return K_BACKSPACE;
    }
    if (codepoint >= 32 && codepoint <= 126)
	return codepoint;
    return IN_TERM_MapFunctionalKey(codepoint);
}

static void
IN_TERM_HandleKey(knum_t key, int mods, int event)
{
    if (key == K_UNKNOWN)
	return;

    /* Emergency quit: three Ctrl+C presses within a second */
    if (key == K_c && (mods & KITTY_MOD_CTRL) && event == KITTY_EVENT_PRESS) {
	double now = Sys_DoubleTime();
	quitpress_times[quitpress_index % 3] = now;
	quitpress_index++;
	if (quitpress_index >= 3 &&
	    now - quitpress_times[quitpress_index % 3] < 1.0)
	    Sys_Quit();
    }

    if (event == KITTY_EVENT_REPEAT) {
	/* Repeats matter for typing, never for game keys or menu toggling */
	if (key_dest == key_game || key == K_ESCAPE)
	    return;
	Key_Event(key, true);
	return;
    }

    Key_Event(key, event != KITTY_EVENT_RELEASE);
}

/* Press with no matching release available (legacy sequences) */
static void
IN_TERM_PulseKey(knum_t key)
{
    if (key == K_UNKNOWN)
	return;
    Key_Event(key, true);
    Key_Event(key, false);
}

/*
 * Parse one CSI sequence: ESC [ params final, where params are integers
 * separated by ';' with ':' introducing sub-parameters.  Returns the
 * length consumed, 0 if incomplete, -1 if not a CSI sequence.
 *
 * Key encodings (kitty protocol):
 *   CSI codepoint ; mods : event u        -- most keys
 *   CSI 1 ; mods : event A/B/C/D/H/F      -- arrows, home, end
 *   CSI 1 ; mods : event P/Q/R/S          -- F1 - F4
 *   CSI number ; mods : event ~           -- ins/del/pgup/pgdn/F5+
 */
static int
IN_TERM_ParseCSI(const char *buf, int len)
{
    int params[4][3] = { {0} };
    int nparam = 0, nsub = 0;
    int i = 2, value = -1;
    char final = 0;
    knum_t key = K_UNKNOWN;
    int mods, event;

    if (len < 2 || buf[0] != '\033' || buf[1] != '[')
	return -1;

    for (; i < len && !final; i++) {
	char ch = buf[i];
	if (ch >= '0' && ch <= '9') {
	    value = (value < 0 ? 0 : value) * 10 + (ch - '0');
	} else if (ch == ';' || ch == ':') {
	    if (nparam < 4 && nsub < 3)
		params[nparam][nsub] = (value < 0) ? 0 : value;
	    value = -1;
	    if (ch == ';') {
		nparam++;
		nsub = 0;
	    } else {
		nsub++;
	    }
	} else if (ch >= 0x40 && ch <= 0x7e) {
	    if (nparam < 4 && nsub < 3)
		params[nparam][nsub] = (value < 0) ? 0 : value;
	    final = ch;
	} else if (ch == '<' || ch == '=' || ch == '>' || ch == '?') {
	    /* private parameter prefix: not a key event, skip to final */
	    for (i++; i < len; i++)
		if (buf[i] >= 0x40 && buf[i] <= 0x7e)
		    return i + 1;
	    return 0;
	} else {
	    return i + 1;	/* malformed, discard */
	}
    }
    if (!final)
	return 0;		/* incomplete, wait for more bytes */

    mods = params[1][0] ? params[1][0] - 1 : 0;
    event = params[1][1] ? params[1][1] : KITTY_EVENT_PRESS;

    switch (final) {
    case 'u':
	key = IN_TERM_MapCodepoint(params[0][0]);
	break;
    case 'A': key = K_UPARROW; break;
    case 'B': key = K_DOWNARROW; break;
    case 'C': key = K_RIGHTARROW; break;
    case 'D': key = K_LEFTARROW; break;
    case 'H': key = K_HOME; break;
    case 'F': key = K_END; break;
    case 'P': key = K_F1; break;
    case 'Q': key = K_F2; break;
    case 'R': key = K_F3; break;
    case 'S': key = K_F4; break;
    case '~':
	switch (params[0][0]) {
	case 2: key = K_INS; break;
	case 3: key = K_DEL; break;
	case 5: key = K_PGUP; break;
	case 6: key = K_PGDN; break;
	case 7: key = K_HOME; break;
	case 8: key = K_END; break;
	case 11: key = K_F1; break;
	case 12: key = K_F2; break;
	case 13: key = K_F3; break;
	case 14: key = K_F4; break;
	case 15: key = K_F5; break;
	case 17: key = K_F6; break;
	case 18: key = K_F7; break;
	case 19: key = K_F8; break;
	case 20: key = K_F9; break;
	case 21: key = K_F10; break;
	case 23: key = K_F11; break;
	case 24: key = K_F12; break;
	}
	break;
    default:
	return i;		/* some other reply; ignore */
    }

    IN_TERM_HandleKey(key, mods, event);
    return i;
}

void
IN_TERM_ProcessInput(void)
{
    int count, consumed;

    count = TTY_ReadInput(in_buf + in_buflen, sizeof(in_buf) - in_buflen);
    in_buflen += count;

    consumed = 0;
    while (consumed < in_buflen) {
	const char *seq = in_buf + consumed;
	int remaining = in_buflen - consumed;

	if (seq[0] != '\033') {
	    /* Legacy byte (shouldn't happen with the kitty protocol) */
	    knum_t key = IN_TERM_MapCodepoint((unsigned char)seq[0]);
	    IN_TERM_PulseKey(key);
	    consumed++;
	    continue;
	}

	if (remaining >= 2 && seq[1] == '[') {
	    int used = IN_TERM_ParseCSI(seq, remaining);
	    if (used == 0)
		break;		/* incomplete sequence, wait for more */
	    consumed += used;
	    in_pending_esc_frames = 0;
	    continue;
	}

	if (remaining >= 2 && seq[1] == 'O' && remaining >= 3) {
	    /* SS3 legacy function keys */
	    knum_t key = K_UNKNOWN;
	    switch (seq[2]) {
	    case 'A': key = K_UPARROW; break;
	    case 'B': key = K_DOWNARROW; break;
	    case 'C': key = K_RIGHTARROW; break;
	    case 'D': key = K_LEFTARROW; break;
	    case 'P': key = K_F1; break;
	    case 'Q': key = K_F2; break;
	    case 'R': key = K_F3; break;
	    case 'S': key = K_F4; break;
	    }
	    IN_TERM_PulseKey(key);
	    consumed += 3;
	    continue;
	}

	if (remaining == 1) {
	    /* Lone ESC: maybe a partial sequence; give it a frame to finish */
	    if (++in_pending_esc_frames < 2)
		break;
	    IN_TERM_PulseKey(K_ESCAPE);
	    in_pending_esc_frames = 0;
	    consumed++;
	    continue;
	}

	/* ESC followed by something unrecognized: discard the ESC */
	consumed++;
    }

    if (consumed) {
	memmove(in_buf, in_buf + consumed, in_buflen - consumed);
	in_buflen -= consumed;
    }
    if (in_buflen == sizeof(in_buf))
	in_buflen = 0;		/* pathological garbage; reset */
}

void
IN_Init(void)
{
    if (!TTY_HasKittyKeyboard()) {
	Sys_Error("This terminal does not support the kitty keyboard protocol,\n"
		  "which is required for key release events (movement keys).\n"
		  "Supported terminals include kitty, ghostty, wezterm, foot and\n"
		  "alacritty >= 0.13.  tmux/screen sessions are not supported.");
    }
    TTY_EnableKittyKeyboard();
}

void
IN_Shutdown(void)
{
}

void
IN_Commands(void)
{
}

void
IN_Move(usercmd_t *cmd)
{
}

void
IN_Accumulate(void)
{
}

void
IN_ModeChanged(void)
{
}

void
IN_AddCommands(void)
{
}

void
IN_RegisterVariables(void)
{
    Cvar_RegisterVariable(&_windowed_mouse);
}
