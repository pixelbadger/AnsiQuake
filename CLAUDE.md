# AnsiQuake

Fork of TyrQuake (upstream remote: `upstream` → https://github.com/sezero/tyrquake)
that renders Quake to ANSI terminals with truecolor half-blocks (`▀`, fg=top
pixel, bg=bottom pixel) and takes input via the kitty keyboard protocol.
See README.md for the user-facing story.

## Build

```sh
make terminal        # = make bin/tyr-quake VID_TARGET=term IN_TARGET=term SND_TARGET=sdl
```

- Needs `libsdl2-dev` (sound only — SDL video is never initialized).
- `DEBUG=Y` for -g builds. **Always `rm -rf build bin` when switching driver
  targets or toggling DEBUG** — all variants share `build/nqsw/` and stale
  objects will silently link.
- Only the software-renderer NQ binary (`bin/tyr-quake`) supports the term
  drivers; GL and QW targets are untouched upstream code.

## Running and testing (headless)

The binary Sys_Errors unless stdin is a tty answering the kitty keyboard
probe (`ESC[?u ESC[c` → expects a `ESC[?...u` reply before the DA1 reply).
Never exec it directly from a non-tty context; use the harness:

```sh
QCOLS=240 QROWS=67 QTIME=25 QOUT=cap.bin \
    scripts/ptyharness.py ./bin/tyr-quake -basedir . -nosound +timedemo demo1
grep fps console.log                       # engine stdout is diverted there
scripts/ansidecode.py cap.bin 240 67 0.5   # capture -> frame_050.png to eyeball
```

- Scripted input: `QSCRIPT=file` with `delay:hexbytes` lines of kitty CSI-u
  sequences (see scripts/ptyharness.py docstring for an ESC example).
- Game data `id1/pak0.pak` is gitignored; restore with
  `scripts/fetch-shareware.sh` (GWDG mirror of ftp.idsoftware.com).
- Performance target: normal play must hold the engine's 72 fps cap
  (`NQ/host.c`, 1/72s frame gate). Reference: ~450 fps timedemo at 240×67.
- Harness fps numbers at very large grids are drain-rate limited (pty
  buffer + 5ms select), so they understate real terminal performance.

## Benchmarking

`QSTATS=file` makes the vid driver append one CSV record per frame
(changed cells, bytes emitted, ANSI-generation µs, tty-write µs, skipped
flag) via raw per-frame `write()` — survives the SIGTERM exit path.
Zero-cost when unset. Tooling on top of it:

- `scripts/benchstats.py stats.csv` — per-run summary (bytes/frame,
  %cells changed, gen vs write time). Auto-trims to the demo window;
  `--json` for machine use, `--all` to skip trimming.
- `scripts/benchmark.py` — headless matrix (grid sizes × timedemo/
  playdemo) via ptyharness; report in `bench/headless-*/summary.md`.
  Byte/cell stats are terminal-independent; write timings are pty drain.
- `scripts/benchterm.py [terms]` — runs timedemo in real emulators on the
  current display, samples terminal+engine CPU from /proc and GPU busy%
  where counters exist (amdgpu sysfs / intel_gpu_top / nvidia-smi);
  report in `bench/terms-*/summary.md`. Needs kitty-protocol terminals
  (alacritty ≥ 0.13 — the script version-checks and skips otherwise).
  Don't run it concurrently with other benchmarks — both share
  `./console.log` to detect timedemo completion.

## Running interactively

`scripts/run-foot.sh` launches a real Foot window sized so the terminal's
pixel grid (cols × 2·rows) is at or above the 320×200 render floor (see
`common/vid_term.c` below) — below that floor the renderer point-samples
down to the physical grid and garbles HUD/console text; at or above it,
1:1 with no scaling. Defaults to a 340×110 char / pixelsize=4 window
(verified to fit a 1920×1080-class display); override with `FOOT_COLS`,
`FOOT_ROWS`, `FOOT_PIXELSIZE` env vars if your display can't fit that, but
keep `FOOT_COLS>=320` and `FOOT_ROWS>=100` — the script refuses to launch
below that. Extra args pass through to `tyr-quake`, e.g.
`scripts/run-foot.sh -nosound +timedemo demo1`.

## Architecture (the fork's code)

- `common/term_tty.c` + `include/term_tty.h` — owns the tty: raw mode, alt
  screen, kitty probe/push/pop, stdout→`console.log` redirect, and
  async-signal-safe restore on every exit path (fatal signals, SIGTSTP/
  SIGCONT, atexit). Anything touching terminal state belongs here.
- `common/vid_term.c` — vid driver. Render resolution = terminal grid
  (cols × 2·rows) clamped to ≥320×200 and width multiple of 8; smaller
  grids downsample via `term_xmap`/`term_ymap`. Emits per-cell diffs with
  SGR state elision inside `CSI ?2026` sync guards; drops at most every
  other frame if the last write exceeded 1/72s. Palette → precomputed
  `"38;2;R;G;B"` fragments in `VID_SetPalette`; `VID_ShiftPalette` (pain
  flashes) marks the whole grid dirty.
- `common/in_term.c` — CSI parser → `Key_Event(knum_t, down)`. Event types:
  1=press, 2=repeat (dropped when `key_dest == key_game`), 3=release.
  Triple Ctrl+C within 1s = emergency quit.
- Makefile: `term` blocks in the VID/IN driver sections; `terminal` phony
  target at the bottom.

Local patches to upstream code (keep in mind when merging upstream):
`common/snd_dma.c` (NULL guard in `S_ClearOverflow` for -nosound),
`common/sys_unix.c` (`Sys_Error` shuts down before printing so the message
survives terminal restore).

## Conventions

- Upstream style: 4-space indent, function name on its own line, id-era
  naming (`VID_*`, `IN_*`, `TTY_*` for the tty layer). Match it.
- Don't add SDL dependencies to the term drivers — the point is that video
  and input are pure termios/ANSI.
