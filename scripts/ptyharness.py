#!/usr/bin/env python3
"""Drive the terminal Quake build through a pty for headless testing.

Sets the pty window size, answers the kitty keyboard protocol probe (so
IN_Init succeeds without a real terminal), captures all ANSI output to a
file, and optionally injects scripted key events.

    usage: ptyharness.py <command...>
    e.g.:  QCOLS=240 QROWS=67 QTIME=25 QOUT=cap.bin \\
               scripts/ptyharness.py ./bin/tyr-quake -basedir . +timedemo demo1

Environment:
    QCOLS/QROWS  terminal grid size (default 200x56)
    QTIME        seconds to run before SIGTERM (default 40)
    QOUT         capture file for the raw ANSI stream (default ptycapture.bin)
    QSCRIPT      optional input script: lines of "delay_seconds:hexbytes",
                 e.g. kitty CSI-u for ESC press+release at t=3s:
                 3.0:1b5b32373b313a31751b5b32373b313a3375

Decode the capture into PNG snapshots with scripts/ansidecode.py.
Engine console output (timedemo fps etc.) lands in ./console.log."""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time

COLS = int(os.environ.get("QCOLS", "200"))
ROWS = int(os.environ.get("QROWS", "56"))
RUNTIME = float(os.environ.get("QTIME", "40"))
OUTFILE = os.environ.get("QOUT", "ptycapture.bin")
# newline-separated list of "delay_seconds:hexbytes" input events
SCRIPTFILE = os.environ.get("QSCRIPT", "")

cmd = sys.argv[1:]
if not cmd:
    sys.exit("usage: ptyharness.py <command...>")

events = []
if SCRIPTFILE:
    for line in open(SCRIPTFILE):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        delay, hexbytes = line.split(":", 1)
        events.append((float(delay), bytes.fromhex(hexbytes)))

master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))

pid = os.fork()
if pid == 0:
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    os.dup2(slave, 0)
    os.dup2(slave, 1)
    os.dup2(slave, 2)
    os.close(master)
    os.close(slave)
    os.execvp(cmd[0], cmd)
    os._exit(127)

os.close(slave)
start = time.time()
probe_answered = False
probe_window = b""
total = 0
exited = None

with open(OUTFILE, "wb") as out:
    while time.time() - start < RUNTIME:
        # deliver scripted input
        while events and time.time() - start >= events[0][0]:
            os.write(master, events.pop(0)[1])
        r, _, _ = select.select([master], [], [], 0.005)
        if master in r:
            try:
                data = os.read(master, 1 << 20)
            except OSError:
                break
            if not data:
                break
            total += len(data)
            out.write(data)
            if not probe_answered:
                probe_window += data[-4096:]
                if b"\x1b[?u\x1b[c" in probe_window:
                    # kitty protocol reply + DA1 reply
                    os.write(master, b"\x1b[?0u\x1b[?62;1;2;4c")
                    probe_answered = True
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            exited = status
            break

if exited is None:
    os.kill(pid, signal.SIGTERM)
    time.sleep(0.5)
    try:
        while True:
            data = os.read(master, 1 << 20)
            if not data:
                break
            total += len(data)
            open(OUTFILE, "ab").write(data)
    except OSError:
        pass
    try:
        os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        pass

print(f"captured {total} bytes over {time.time()-start:.1f}s, "
      f"probe_answered={probe_answered}, exit={exited}")
