# AnsiQuake — Quake in your terminal

A fork of [TyrQuake](https://disenchant.net/tyrquake/) that renders Quake to an
ANSI terminal at original game speed. Every character cell displays two pixels
using the Unicode half block `▀` (U+2580): the foreground color carries the top
pixel and the background color the bottom pixel, doubling the vertical
resolution of the terminal grid. Truecolor SGR sequences, per-cell frame
diffing, and synchronized updates (`CSI ?2026`) keep the output stream small
and tear-free.

## Requirements

- A terminal with **truecolor** and the **kitty keyboard protocol**:
  kitty, ghostty, wezterm, foot, or alacritty ≥ 0.13.
  (The protocol is required for key *release* events — without it, WASD
  movement is impossible. tmux/screen sessions are not supported.)
- Linux, gcc, make; `libsdl2-dev` for sound.
- Quake game data (`id1/pak0.pak`). No data? `scripts/fetch-shareware.sh`
  downloads the freely redistributable shareware episode.

## Build & run

```sh
make terminal            # builds bin/tyr-quake with the terminal drivers
scripts/fetch-shareware.sh
./bin/tyr-quake
```

The render resolution is dictated by your terminal: one pixel per column,
two per row. Below 320×200 grid pixels the engine renders at 320×200 and
downsamples. Recommended: a maximized terminal at a small font size
(≥ 160×50 cells; ~240×70 looks great). Resizing mid-game works.

Engine console output is diverted to `./console.log` while the display is
active. Quit from the menu (Escape), or press Ctrl+C three times within a
second for an emergency exit. The terminal is restored even on crashes,
`kill`, or Ctrl+Z.

## How it works

- `common/vid_term.c` — video driver: samples the 8-bit palettized software
  framebuffer into a `(top, bottom)` cell grid, diffs against the previous
  frame, and emits only changed cells with run-elided SGR state (color
  strings are precomputed per palette index). If the tty can't keep pace,
  at most every other frame is dropped so the engine never slows down.
- `common/in_term.c` — input driver: parses kitty-protocol CSI sequences
  into Quake key press/release events.
- `common/term_tty.c` — raw mode, alternate screen, capability probing, and
  crash-safe terminal restore (fatal signals, SIGTSTP/SIGCONT, SIGWINCH).

Everything else is stock TyrQuake (software renderer lineage: WinQuake).

## Development & testing

The game refuses to start unless stdin is a terminal that answers the kitty
keyboard protocol probe, so headless testing goes through two small,
dependency-light Python scripts (no test framework, no fixtures — just
pty + regex).

`scripts/ptyharness.py` forks the given command onto one end of a real
pty (`pty.openpty()`), sets its window size via `TIOCSWINSZ` to
`QCOLS`x`QROWS`, and pumps the pty in a select loop for `QTIME` seconds,
writing every byte the child emits to `QOUT`. The first time it sees the
kitty probe sequence (`ESC[?u ESC[c`) in the output, it writes back
`ESC[?0u ESC[?62;1;2;4c` (kitty-protocol reply + a DA1 reply), which is
enough for `IN_Init` to proceed as if a real terminal answered. If
`QSCRIPT` points at a file, each line `delay_seconds:hexbytes` is queued
and written to the pty's input side at that offset from start — that's
how scripted key presses (kitty CSI-u encoding) get injected without a
human at the keyboard. On timeout it SIGTERMs the child and drains any
remaining output before exiting.

```sh
# Benchmark: run the standard timedemo at a 240x67 cell grid for 25s.
# The fps line lands in ./console.log (stdout is diverted there in-game).
QCOLS=240 QROWS=67 QTIME=25 QOUT=cap.bin \
    scripts/ptyharness.py ./bin/tyr-quake -basedir . -nosound +timedemo demo1
grep fps console.log

# Drive the menu: send scripted keys (kitty CSI-u encoding, hex) —
# ESC press+release at t=3s, Down arrow at t=4s.
printf '%s\n' \
    '3.0:1b5b32373b313a31751b5b32373b313a3375' \
    '4.0:1b5b313b313a31421b5b313b313a3342' > input.txt
QSCRIPT=input.txt QTIME=7 scripts/ptyharness.py ./bin/tyr-quake -basedir .
```

`scripts/ansidecode.py` turns a raw capture back into something you can
actually look at. It's a deliberately minimal terminal emulator — just
enough of the ANSI subset the video driver emits: cursor positioning
(`CUP`), truecolor SGR (`38;2;…`/`48;2;…`), and erase-display (`ED`) reset
the half-block grid it tracks internally; alt-screen and `CSI ?2026`
sync-guard sequences are parsed but ignored since they don't affect
pixel content. For each fraction in `points` (of total bytes captured) it
replays the stream up to that offset, reconstructs the `(char, fg, bg)`
grid, and rasterizes each cell's foreground/background as its top/bottom
pixel into a 3x-upscaled `frame_NNN.png` (`NNN` = percent through the
stream). Needs `python3-pil`.

```sh
# Inspect what was actually drawn: decode the capture into PNG snapshots
# at 30%/70%/98% of the stream -> frame_030.png, frame_070.png, frame_098.png
scripts/ansidecode.py cap.bin 240 67 0.3,0.7,0.98
```

Baseline numbers (2026, 8-core x86-64, pty harness): `timedemo demo1` runs
332 fps at 240×67 cells and ~72 fps at an extreme 800×200; normal play is
capped by the engine at 72 fps, the original Quake speed limit. Harness fps
at very large grids is drain-rate limited (pty buffer + select polling), so
it understates real terminal performance.

To play interactively rather than run headless, `scripts/run-foot.sh`
launches a real [foot](https://codeberg.org/dnkl/foot) window pre-sized so
its pixel grid clears the 320×200 render floor above, avoiding the garbled
HUD/console text a too-small window would produce. Extra args pass through
to `tyr-quake`, e.g. `scripts/run-foot.sh -nosound +timedemo demo1`.

Debug build: `make bin/tyr-quake VID_TARGET=term IN_TARGET=term
SND_TARGET=sdl DEBUG=Y`. Always `rm -rf build bin` when switching targets
or toggling DEBUG — objects land in the same build directory. Upstream is
tracked on the `upstream` remote (sezero/tyrquake) for future merges.

## License

GPLv2, same as the id Software Quake source release and TyrQuake.
