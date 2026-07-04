#!/usr/bin/env python3
"""Cross-terminal benchmark: run timedemo in real terminal emulators.

Launches bin/tyr-quake +timedemo demo1 inside each requested terminal
emulator on the current display, with QSTATS per-frame instrumentation,
and samples /proc CPU time of both the terminal and the engine while the
demo runs. Where GPU utilization counters exist (amdgpu sysfs,
intel_gpu_top, nvidia-smi) the GPU busy percentage is sampled too, to
separate CPU-bound from GPU-bound terminals.

Per terminal this answers: does the emulator keep up (engine fps vs
headless baseline, dropped frames, write-stall time), and what does
keeping up cost (terminal CPU% of one core, GPU busy%)?

Requirements: a display, and terminals that speak the kitty keyboard
protocol (foot, kitty, wezterm, ghostty, alacritty >= 0.13). xterm and
friends cannot run the game at all.

    usage: benchterm.py [terminal ...]     (default: all installed)
    env:   BCOLS/BROWS  requested grid (default 340x110)
"""
import datetime
import os
import re
import shutil
import signal
import subprocess
import sys
import time

from benchstats import summarize

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COLS = int(os.environ.get("BCOLS", "340"))
ROWS = int(os.environ.get("BROWS", "110"))
HZ = os.sysconf("SC_CLK_TCK")
CONSOLE = os.path.join(REPO, "console.log")

# {cmd} is replaced by the inner command list. Font sizes are chosen so a
# 340-column window fits a ~1920px-wide display.
LAUNCHERS = {
    "foot": ["foot", "-f", "monospace:pixelsize=4",
             "-W", f"{COLS}x{ROWS}", "-e"],
    "kitty": ["kitty", "-o", "font_size=3",
              "-o", f"initial_window_width={COLS}c",
              "-o", f"initial_window_height={ROWS}c"],
    "alacritty": ["alacritty",
                  "-o", f"window.dimensions.columns={COLS}",
                  "-o", f"window.dimensions.lines={ROWS}",
                  "-o", "font.size=3", "-e"],
    "wezterm": ["wezterm", "--config", "font_size=3",
                "--config", f"initial_cols={COLS}",
                "--config", f"initial_rows={ROWS}", "start", "--"],
    "ghostty": ["ghostty", "--font-size=3",
                f"--window-width={COLS}", f"--window-height={ROWS}", "-e"],
}


def proc_ticks(pid):
    """utime+stime of pid in clock ticks, or None if gone."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12])
    except (OSError, IndexError):
        return None


def find_quake_pid(deadline):
    # match on comm, not -f: the terminal's own cmdline contains the
    # binary path and would match (and can win the race before exec)
    while time.time() < deadline:
        r = subprocess.run(["pgrep", "-n", "-x", "tyr-quake"],
                           capture_output=True, text=True)
        if r.stdout.strip():
            return int(r.stdout.split()[0])
        time.sleep(0.1)
    return None


class GpuSampler:
    """Best-effort GPU busy%% sampling; .stop() returns mean %% or None."""

    def __init__(self):
        self.kind = None
        self.samples = []
        self.proc = None
        self.amd = None
        for p in ["/sys/class/drm/card0/device/gpu_busy_percent",
                  "/sys/class/drm/card1/device/gpu_busy_percent"]:
            if os.path.exists(p):
                self.kind, self.amd = "amdgpu", p
                return
        if shutil.which("intel_gpu_top"):
            try:
                self.proc = subprocess.Popen(
                    ["sudo", "-n", "intel_gpu_top", "-J", "-s", "500"],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                self.kind = "intel"
                return
            except OSError:
                pass
        if shutil.which("nvidia-smi"):
            self.kind = "nvidia"

    def poll(self):
        if self.kind == "amdgpu":
            try:
                self.samples.append(float(open(self.amd).read()))
            except (OSError, ValueError):
                pass
        elif self.kind == "nvidia":
            r = subprocess.run(
                ["nvidia-smi", "--query-gpu=utilization.gpu",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True)
            try:
                self.samples.append(float(r.stdout.strip().split("\n")[0]))
            except ValueError:
                pass

    def stop(self):
        if self.kind == "intel" and self.proc:
            self.proc.terminate()
            out = self.proc.stdout.read().decode(errors="replace")
            self.samples = [float(x) for x in re.findall(
                r'"Render/3D".*?"busy"\s*:\s*([\d.]+)', out, re.S)]
        if not self.samples:
            return None
        return round(sum(self.samples) / len(self.samples), 1)


def bench_one(term, outdir):
    stats = os.path.join(outdir, f"{term}.csv")
    for f in (stats, CONSOLE):
        if os.path.exists(f):
            os.unlink(f)

    inner = [os.path.join(REPO, "bin/tyr-quake"), "-basedir", REPO,
             "-nosound", "+timedemo", "demo1"]
    env = dict(os.environ, QSTATS=stats)
    tproc = subprocess.Popen(LAUNCHERS[term] + inner, env=env, cwd=REPO,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    result = {"terminal": term}

    qpid = find_quake_pid(time.time() + 15)
    if qpid is None:
        tproc.terminate()
        result["error"] = "game did not start"
        return result

    gpu = GpuSampler()
    samples = []          # (t, term_ticks, quake_ticks)
    deadline = time.time() + 150
    fps = None
    while time.time() < deadline:
        tt, qt = proc_ticks(tproc.pid), proc_ticks(qpid)
        if tt is None or qt is None:
            break         # someone died (crash or Sys_Error)
        samples.append((time.time(), tt, qt))
        gpu.poll()
        try:
            m = re.search(r"(\d+) frames +[\d.]+ seconds +([\d.]+) fps",
                          open(CONSOLE, errors="replace").read())
        except OSError:
            m = None
        if m:
            fps = float(m.group(2))
            break
        time.sleep(0.25)

    result["gpu_pct"] = gpu.stop()
    # skip the first 2s (startup, intro) when computing CPU%
    t0 = samples[0][0] if samples else 0
    window = [s for s in samples if s[0] - t0 >= 2.0]
    if len(window) >= 2:
        wall = window[-1][0] - window[0][0]
        result["term_cpu_pct"] = round(
            (window[-1][1] - window[0][1]) / HZ / wall * 100, 1)
        result["quake_cpu_pct"] = round(
            (window[-1][2] - window[0][2]) / HZ / wall * 100, 1)

    if os.path.exists(f"/proc/{qpid}"):
        os.kill(qpid, signal.SIGTERM)
    try:
        tproc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        tproc.kill()

    if fps is None:
        result["error"] = "timedemo did not finish"
        return result
    result["timedemo_fps"] = fps
    try:
        result.update(summarize(stats))
    except (ValueError, OSError) as e:
        result["error"] = str(e)
    return result


def verdict(r):
    if "error" in r:
        return r["error"]
    if r.get("term_cpu_pct", 0) >= 90:
        return "terminal CPU-bound (core saturated)"
    if r.get("gpu_pct") is not None and r["gpu_pct"] >= 90:
        return "terminal GPU-bound"
    if r.get("quake_cpu_pct", 0) >= 90:
        return "engine-bound"
    return "not saturated (pipeline/vsync paced)"


def main():
    wanted = sys.argv[1:] or list(LAUNCHERS)
    runnable = []
    for t in wanted:
        if t not in LAUNCHERS:
            sys.exit(f"unknown terminal {t!r} (know: {', '.join(LAUNCHERS)})")
        if not shutil.which(t):
            print(f"skipping {t}: not installed")
            continue
        if t == "alacritty":
            v = subprocess.run(["alacritty", "--version"],
                               capture_output=True, text=True).stdout
            m = re.search(r"(\d+)\.(\d+)", v)
            if m and (int(m.group(1)), int(m.group(2))) < (0, 13):
                print(f"skipping alacritty {m.group(0)}: kitty keyboard "
                      "protocol needs >= 0.13")
                continue
        runnable.append(t)
    if not runnable:
        sys.exit("no runnable terminals")

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(REPO, "bench", f"terms-{stamp}")
    os.makedirs(outdir, exist_ok=True)

    results = []
    for t in runnable:
        print(f"[{t}] running timedemo ...", flush=True)
        r = bench_one(t, outdir)
        results.append(r)
        print(f"    {r.get('timedemo_fps', '-')} fps, "
              f"term cpu {r.get('term_cpu_pct', '-')}%, "
              f"quake cpu {r.get('quake_cpu_pct', '-')}%, "
              f"gpu {r['gpu_pct'] if r.get('gpu_pct') is not None else 'n/a'}"
              f" -> {verdict(r)}")
        time.sleep(1)

    cols = [("terminal", "terminal"), ("grid", "grid"),
            ("timedemo_fps", "timedemo fps"), ("skipped_pct", "dropped %"),
            ("mb_per_s", "MB/s"), ("write_us_mean", "write us mean"),
            ("write_time_pct", "write % of wall"),
            ("quake_cpu_pct", "quake CPU %"), ("term_cpu_pct", "term CPU %"),
            ("gpu_pct", "GPU %")]
    lines = [f"# Terminal emulator benchmark {stamp}",
             f"grid requested {COLS}x{ROWS}; timedemo demo1", "",
             "| " + " | ".join(h for _, h in cols) + " | verdict |",
             "|" + "---|" * (len(cols) + 1)]
    for r in results:
        vals = [str(r.get(k, "-")) if r.get(k) is not None else "n/a"
                for k, _ in cols]
        lines.append("| " + " | ".join(vals) + f" | {verdict(r)} |")
    report = "\n".join(lines) + "\n"
    path = os.path.join(outdir, "summary.md")
    with open(path, "w") as f:
        f.write(report)
    print(f"\n{report}\nwritten to {path}")


if __name__ == "__main__":
    main()
