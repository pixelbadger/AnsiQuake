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
// term_tty.c -- raw terminal setup/teardown shared by vid_term and in_term

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "common.h"
#include "sys.h"
#include "term_tty.h"

static int tty_out_fd = -1;
static int tty_in_fd = -1;
static int tty_saved_stdout = -1;

static struct termios tty_saved_termios;
static struct termios tty_raw_termios;

static qboolean tty_active;
static qboolean tty_has_kitty;
static qboolean tty_kitty_pushed;

static volatile sig_atomic_t tty_resized;
static volatile sig_atomic_t tty_resumed;

/* enter alt screen, hide cursor, disable autowrap, clear */
#define TTY_SETUP_SEQ	"\033[?1049h\033[?25l\033[?7l\033[2J\033[H"
/* reset SGR, re-enable autowrap, show cursor, leave alt screen */
#define TTY_RESTORE_SEQ	"\033[0m\033[?7h\033[?25h\033[?1049l"
/* kitty keyboard protocol: disambiguate (1) + event types (2) + all keys as escapes (8) */
#define TTY_KITTY_PUSH	"\033[>11u"
#define TTY_KITTY_POP	"\033[<u"

static void
TTY_RawWrite(int fd, const char *buf, size_t len)
{
    while (len) {
	ssize_t written = write(fd, buf, len);
	if (written < 0) {
	    if (errno == EINTR || errno == EAGAIN)
		continue;
	    return;
	}
	buf += written;
	len -= written;
    }
}

/*
 * Restore the terminal using only async-signal-safe calls so it can run
 * from fatal signal handlers.  A crashed game must never leave the tty
 * in raw mode on the alternate screen.
 */
static void
TTY_EmergencyRestore(void)
{
    if (!tty_active)
	return;
    if (tty_kitty_pushed)
	TTY_RawWrite(tty_out_fd, TTY_KITTY_POP, sizeof(TTY_KITTY_POP) - 1);
    TTY_RawWrite(tty_out_fd, TTY_RESTORE_SEQ, sizeof(TTY_RESTORE_SEQ) - 1);
    tcsetattr(tty_in_fd, TCSANOW, &tty_saved_termios);
}

static void
TTY_FatalSignal(int sig)
{
    TTY_EmergencyRestore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void
TTY_SigWinch(int sig)
{
    tty_resized = 1;
}

static void
TTY_SigTstp(int sig)
{
    TTY_EmergencyRestore();
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
}

static void
TTY_SigCont(int sig)
{
    if (!tty_active)
	return;
    tcsetattr(tty_in_fd, TCSANOW, &tty_raw_termios);
    TTY_RawWrite(tty_out_fd, TTY_SETUP_SEQ, sizeof(TTY_SETUP_SEQ) - 1);
    if (tty_kitty_pushed)
	TTY_RawWrite(tty_out_fd, TTY_KITTY_PUSH, sizeof(TTY_KITTY_PUSH) - 1);
    signal(SIGTSTP, TTY_SigTstp);
    tty_resumed = 1;
    tty_resized = 1;	/* size may have changed while stopped */
}

/*
 * Query kitty keyboard protocol support: the protocol query is followed
 * by a DA1 request; every terminal answers DA1, so if the kitty reply
 * (CSI ? flags u) arrives before the DA1 reply (CSI ? ... c) we know the
 * protocol is available.
 */
static void
TTY_ProbeKittyKeyboard(void)
{
    static const char query[] = "\033[?u\033[c";
    char buf[256];
    int len = 0;
    int timeout = 1000;

    TTY_RawWrite(tty_out_fd, query, sizeof(query) - 1);

    while (len < (int)sizeof(buf) - 1) {
	struct pollfd pfd = { .fd = tty_in_fd, .events = POLLIN };
	int err = poll(&pfd, 1, timeout);
	if (err <= 0)
	    return;
	ssize_t count = read(tty_in_fd, buf + len, sizeof(buf) - 1 - len);
	if (count <= 0)
	    return;
	len += count;
	buf[len] = 0;

	/* Scan for complete CSI replies */
	for (char *csi = buf; (csi = strstr(csi, "\033[?")) != NULL; csi++) {
	    char *end = csi + 3;
	    while (*end && (*end == ';' || (*end >= '0' && *end <= '9')))
		end++;
	    if (!*end)
		break;		/* incomplete, read more */
	    if (*end == 'u')
		tty_has_kitty = true;
	    if (*end == 'c')
		return;		/* DA1 terminates the probe */
	}
	timeout = 200;
    }
}

void
TTY_Init(void)
{
    int logfd;

    if (tty_active)
	return;

    if (!isatty(STDIN_FILENO))
	Sys_Error("stdin is not a terminal");

    tty_in_fd = STDIN_FILENO;
    tty_out_fd = open("/dev/tty", O_WRONLY);
    if (tty_out_fd < 0)
	tty_out_fd = dup(STDOUT_FILENO);

    if (tcgetattr(tty_in_fd, &tty_saved_termios))
	Sys_Error("%s: tcgetattr: %s", __func__, strerror(errno));

    tty_raw_termios = tty_saved_termios;
    cfmakeraw(&tty_raw_termios);
    tty_raw_termios.c_cc[VMIN] = 0;
    tty_raw_termios.c_cc[VTIME] = 0;
    tcsetattr(tty_in_fd, TCSANOW, &tty_raw_termios);

    /*
     * Console output on stdout would corrupt the display; divert it to a
     * log file for the lifetime of the terminal display.
     */
    tty_saved_stdout = dup(STDOUT_FILENO);
    logfd = open("console.log", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (logfd >= 0) {
	dup2(logfd, STDOUT_FILENO);
	close(logfd);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    tty_active = true;

    TTY_ProbeKittyKeyboard();
    TTY_RawWrite(tty_out_fd, TTY_SETUP_SEQ, sizeof(TTY_SETUP_SEQ) - 1);

    signal(SIGWINCH, TTY_SigWinch);
    signal(SIGTSTP, TTY_SigTstp);
    signal(SIGCONT, TTY_SigCont);
    signal(SIGSEGV, TTY_FatalSignal);
    signal(SIGBUS, TTY_FatalSignal);
    signal(SIGFPE, TTY_FatalSignal);
    signal(SIGILL, TTY_FatalSignal);
    signal(SIGABRT, TTY_FatalSignal);
    signal(SIGTERM, TTY_FatalSignal);
    signal(SIGHUP, TTY_FatalSignal);

    atexit(TTY_Shutdown);
}

void
TTY_Shutdown(void)
{
    if (!tty_active)
	return;
    TTY_EmergencyRestore();
    tty_active = false;
    if (tty_saved_stdout >= 0) {
	fflush(stdout);
	dup2(tty_saved_stdout, STDOUT_FILENO);
	close(tty_saved_stdout);
	tty_saved_stdout = -1;
    }
}

qboolean
TTY_IsActive(void)
{
    return tty_active;
}

qboolean
TTY_HasKittyKeyboard(void)
{
    return tty_has_kitty;
}

void
TTY_EnableKittyKeyboard(void)
{
    TTY_RawWrite(tty_out_fd, TTY_KITTY_PUSH, sizeof(TTY_KITTY_PUSH) - 1);
    tty_kitty_pushed = true;
}

void
TTY_GetSize(int *cols, int *rows)
{
    struct winsize ws;

    if (!ioctl(tty_out_fd, TIOCGWINSZ, &ws) && ws.ws_col && ws.ws_row) {
	*cols = ws.ws_col;
	*rows = ws.ws_row;
    } else {
	*cols = 80;
	*rows = 24;
    }
}

qboolean
TTY_CheckResize(void)
{
    if (!tty_resized)
	return false;
    tty_resized = 0;
    return true;
}

qboolean
TTY_CheckResume(void)
{
    if (!tty_resumed)
	return false;
    tty_resumed = 0;
    return true;
}

void
TTY_Write(const void *buf, size_t len)
{
    TTY_RawWrite(tty_out_fd, buf, len);
}

int
TTY_ReadInput(void *buf, size_t maxlen)
{
    ssize_t count = read(tty_in_fd, buf, maxlen);
    return (count < 0) ? 0 : count;
}
