# SpectreDoom — Quake in your terminal

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

## License

GPLv2, same as the id Software Quake source release and TyrQuake.
