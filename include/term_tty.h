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
// term_tty.h -- shared tty state for the ANSI terminal video/input drivers

#ifndef TERM_TTY_H
#define TERM_TTY_H

#include <stddef.h>

#include "qtypes.h"

void TTY_Init(void);
void TTY_Shutdown(void);
qboolean TTY_IsActive(void);

qboolean TTY_HasKittyKeyboard(void);
void TTY_EnableKittyKeyboard(void);

void TTY_GetSize(int *cols, int *rows);
qboolean TTY_CheckResize(void);	/* true once after SIGWINCH */
qboolean TTY_CheckResume(void);	/* true once after SIGCONT re-init */

void TTY_Write(const void *buf, size_t len);
int TTY_ReadInput(void *buf, size_t maxlen);	/* non-blocking, returns bytes read */

/* implemented by in_term.c, called from the vid driver's event processing */
void IN_TERM_ProcessInput(void);

#endif /* TERM_TTY_H */
