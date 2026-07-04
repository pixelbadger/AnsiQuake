#!/usr/bin/env python3
"""Summarize a QSTATS per-frame CSV from the terminal video driver.

The driver (common/vid_term.c) writes one record per VID_Update call when
QSTATS=<file> is set:

    # cols=240 rows=67 render=320x200
    frame,ms,changed,bytes,gen_us,write_us,skipped

    changed   cells re-emitted this frame (of cols*rows total)
    bytes     ANSI bytes written for the frame (0 = nothing changed)
    gen_us    time diffing the grid + encoding ANSI into the out buffer
    write_us  time blocked in write() to the tty
    skipped   1 = frame dropped because the previous write blew the
              1/72s budget (terminal backpressure)

A capture usually brackets the interesting part (demo playback) with
console/startup idle. Unless --all is given, analysis is trimmed to the
span between the first and last run of >= 10 consecutive "active" frames
(bytes > 0 or skipped), which isolates the demo from console idle where
only the blinking cursor emits output.

    usage: benchstats.py stats.csv [--json] [--all] [--console[=file]]
"""
import json
import os
import re
import sys


def percentile(sorted_vals, p):
    if not sorted_vals:
        return 0
    k = min(len(sorted_vals) - 1, int(round(p / 100 * (len(sorted_vals) - 1))))
    return sorted_vals[k]


def load(path):
    cols = rows = renderw = renderh = 0
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#"):
                m = re.search(r"cols=(\d+) rows=(\d+) render=(\d+)x(\d+)", line)
                if m:
                    cols, rows, renderw, renderh = map(int, m.groups())
                continue
            if not line or line.startswith("frame,"):
                continue
            f_, ms, changed, nbytes, gen, wr, skip = line.split(",")
            recs.append((float(ms), int(changed), int(nbytes),
                         int(gen), int(wr), int(skip)))
    return cols, rows, renderw, renderh, recs


def demo_window(recs, minrun=10):
    """Indices [a, b) spanning first->last run of >= minrun active frames."""
    active = [r[2] > 0 or r[5] for r in recs]
    runs = []
    start = None
    for i, a in enumerate(active + [False]):
        if a and start is None:
            start = i
        elif not a and start is not None:
            if i - start >= minrun:
                runs.append((start, i))
            start = None
    if not runs:
        return 0, len(recs)
    return runs[0][0], runs[-1][1]


def summarize(path, console=None, whole=False):
    """Analyze a QSTATS csv; returns a flat dict of metrics."""
    cols, rows, renderw, renderh, recs = load(path)
    if not recs:
        raise ValueError(f"{path}: no records")
    a, b = (0, len(recs)) if whole else demo_window(recs)
    span = recs[a:b]
    dur = (span[-1][0] - span[0][0]) / 1000 or 1e-9
    total_cells = cols * rows

    drawn = [r for r in span if r[2] > 0]
    skipped = sum(r[5] for r in span)
    nbytes = sorted(r[2] for r in drawn)
    pct = sorted(100 * r[1] / total_cells for r in drawn)
    gen = sorted(r[3] for r in drawn)
    wr = sorted(r[4] for r in drawn)
    total_bytes = sum(nbytes)
    total_gen = sum(r[3] for r in span) / 1e6
    total_wr = sum(r[4] for r in span) / 1e6

    out = {
        "grid": f"{cols}x{rows}",
        "render": f"{renderw}x{renderh}",
        "duration_s": round(dur, 2),
        "frames": len(span),
        "fps": round(len(span) / dur, 1),
        "drawn": len(drawn),
        "skipped": skipped,
        "skipped_pct": round(100 * skipped / len(span), 1),
        "bytes_mean": int(sum(nbytes) / len(nbytes)) if nbytes else 0,
        "bytes_p50": percentile(nbytes, 50),
        "bytes_p95": percentile(nbytes, 95),
        "bytes_max": nbytes[-1] if nbytes else 0,
        "mb_total": round(total_bytes / 1e6, 1),
        "mb_per_s": round(total_bytes / 1e6 / dur, 1),
        "cells_pct_mean": round(sum(pct) / len(pct), 1) if pct else 0,
        "cells_pct_p50": round(percentile(pct, 50), 1),
        "cells_pct_p95": round(percentile(pct, 95), 1),
        "bytes_per_cell": round(total_bytes / max(1, sum(r[1] for r in drawn)), 1),
        "gen_us_mean": int(sum(gen) / len(gen)) if gen else 0,
        "gen_us_p95": percentile(gen, 95),
        "write_us_mean": int(sum(wr) / len(wr)) if wr else 0,
        "write_us_p95": percentile(wr, 95),
        "gen_time_pct": round(100 * total_gen / dur, 1),
        "write_time_pct": round(100 * total_wr / dur, 1),
    }
    if console and os.path.exists(console):
        m = re.search(r"(\d+) frames +([\d.]+) seconds +([\d.]+) fps",
                      open(console, errors="replace").read())
        if m:
            out["timedemo_fps"] = float(m.group(3))
    return out


def format_summary(p):
    lines = [f"grid {p['grid']} cells, render {p['render']}, "
             f"{p['duration_s']}s analyzed ({p['frames']} frames)"]
    if "timedemo_fps" in p:
        lines.append(f"timedemo:      {p['timedemo_fps']} fps (engine-reported)")
    lines += [
        f"frame rate:    {p['fps']} fps engine, {p['drawn']} drawn, "
        f"{p['skipped']} dropped ({p['skipped_pct']}%)",
        f"bytes/frame:   mean {p['bytes_mean']:,}  p50 {p['bytes_p50']:,}  "
        f"p95 {p['bytes_p95']:,}  max {p['bytes_max']:,}",
        f"stream:        {p['mb_total']} MB total, {p['mb_per_s']} MB/s, "
        f"{p['bytes_per_cell']} bytes per changed cell",
        f"cells changed: mean {p['cells_pct_mean']}%  "
        f"p50 {p['cells_pct_p50']}%  p95 {p['cells_pct_p95']}%",
        f"ansi gen:      mean {p['gen_us_mean']} us/frame  "
        f"p95 {p['gen_us_p95']} us  ({p['gen_time_pct']}% of wall time)",
        f"tty write:     mean {p['write_us_mean']} us/frame  "
        f"p95 {p['write_us_p95']} us  ({p['write_time_pct']}% of wall time)",
    ]
    return "\n".join(lines)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    console = None
    for o in opts:
        if o.startswith("--console"):
            console = o.split("=", 1)[1] if "=" in o else "console.log"

    out = summarize(args[0], console=console, whole="--all" in opts)
    if "--json" in opts:
        print(json.dumps(out, indent=2))
    else:
        print(format_summary(out))


if __name__ == "__main__":
    main()
