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
// term_intro.h -- demoscene-style ANSI text intro, played once at startup

#ifndef TERM_INTRO_H
#define TERM_INTRO_H

#include "qtypes.h"

/*
 * Renders an animated ANSI demoscene intro directly to the tty (plasma
 * background + block-art logo + text caption), blocking until it finishes
 * or the user presses a key. On return, hands back a malloc'd RGB888
 * snapshot of the final displayed frame -- row-major, top to bottom, 3
 * bytes per pixel, (*out_cols) wide by (*out_rows)*2 tall -- for the vid
 * driver to melt away into the first real game frame. The caller owns the
 * returned buffer. Returns NULL (drawing nothing) if the terminal is too
 * small to bother with.
 */
byte *TERM_PlayIntro(int *out_cols, int *out_rows);

#endif /* TERM_INTRO_H */
