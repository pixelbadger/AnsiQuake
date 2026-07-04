#!/usr/bin/env python3
"""Headless benchmark matrix: grid sizes x {timedemo, playdemo}.

Runs bin/tyr-quake under scripts/ptyharness.py with QSTATS per-frame
instrumentation at several terminal grid sizes, in two modes:

    timedemo  demo1 rendered as fast as possible -- the stress case,
              measures peak encode + emit throughput
    playdemo  demo1 played back in real time at the engine's 72 fps
              cap -- the realistic "actual gameplay" case

Results land in bench/headless-<timestamp>/ as per-run CSVs plus a
summary.md table. Caveat: pty write timings measure drain into a kernel
pty buffer polled by the harness, not a real terminal; use
scripts/benchterm.py for end-to-end numbers against real emulators.

    usage: benchmark.py [--quick]      (--quick = one size per mode)
"""
import datetime
import os
import subprocess
import sys

from benchstats import summarize

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [(240, 67), (340, 110), (800, 200)]
# playdemo runs in real time; timedemo ends on its own (fps line printed)
MODES = [("timedemo", "+timedemo demo1", 45), ("playdemo", "+playdemo demo1", 30)]

COLUMNS = [
    ("grid", "grid"), ("mode", "mode"), ("fps", "engine fps"),
    ("skipped_pct", "dropped %"), ("bytes_p50", "bytes/frame p50"),
    ("bytes_p95", "p95"), ("mb_per_s", "MB/s"),
    ("cells_pct_p50", "cells changed p50 %"),
    ("gen_us_mean", "ansi gen us"), ("write_us_mean", "pty write us"),
]


def run(cols, rows, mode_args, seconds, stats_path):
    env = dict(os.environ, QCOLS=str(cols), QROWS=str(rows),
               QTIME=str(seconds), QOUT="/dev/null", QSTATS=stats_path)
    cmd = [os.path.join(REPO, "scripts/ptyharness.py"),
           os.path.join(REPO, "bin/tyr-quake"),
           "-basedir", REPO, "-nosound"] + mode_args.split()
    subprocess.run(cmd, env=env, cwd=REPO, check=True,
                   stdout=subprocess.DEVNULL)


def main():
    quick = "--quick" in sys.argv
    sizes = SIZES[:1] if quick else SIZES
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(REPO, "bench", f"headless-{stamp}")
    os.makedirs(outdir, exist_ok=True)

    results = []
    for cols, rows in sizes:
        for name, args, seconds in MODES:
            stats = os.path.join(outdir, f"{name}-{cols}x{rows}.csv")
            print(f"[{cols}x{rows} {name}] running {seconds}s ...",
                  flush=True)
            run(cols, rows, args, seconds, stats)
            s = summarize(stats, console=os.path.join(REPO, "console.log"))
            s["mode"] = name
            results.append(s)
            print("    " + f"{s['fps']} fps, {s['mb_per_s']} MB/s, "
                  f"{s['cells_pct_p50']}% cells/frame")

    lines = [f"# Headless benchmark {stamp}", "",
             "Byte counts and cell-diff stats are terminal-independent; "
             "write timings here are pty drain, not a real terminal.", "",
             "| " + " | ".join(h for _, h in COLUMNS) + " |",
             "|" + "---|" * len(COLUMNS)]
    for s in results:
        lines.append("| " + " | ".join(str(s.get(k, "")) for k, _ in COLUMNS)
                     + " |")
    report = "\n".join(lines) + "\n"
    path = os.path.join(outdir, "summary.md")
    with open(path, "w") as f:
        f.write(report)
    print(f"\n{report}\nwritten to {path}")


if __name__ == "__main__":
    main()
