#!/usr/bin/env python3
"""Decode a captured ANSI half-block stream into PNG snapshots.

    usage: ansidecode.py <capture.bin> [cols] [rows] [points]

cols/rows must match the QCOLS/QROWS the capture was made with (default
200x56). points is a comma-separated list of fractions of the stream at
which to snapshot (default "0.5,0.98"); each writes frame_NNN.png at 3x
scale. Requires python3-pil.

Emulates just enough of a terminal: CUP, truecolor SGR, ED; alt-screen
and synchronized-update sequences are ignored."""
import re
import sys

from PIL import Image

COLS = int(sys.argv[2]) if len(sys.argv) > 2 else 200
ROWS = int(sys.argv[3]) if len(sys.argv) > 3 else 56
# fractions of the stream at which to snapshot
POINTS = [float(x) for x in (sys.argv[4].split(",") if len(sys.argv) > 4 else ["0.5", "0.98"])]

data = open(sys.argv[1], "rb").read()

def rasterize(grid, name):
    img = Image.new("RGB", (COLS, ROWS * 2))
    px = img.load()
    for r in range(ROWS):
        for c in range(COLS):
            ch, fg, bg = grid[r][c]
            top = fg if ch == "▀" else bg
            px[c, r * 2] = top
            px[c, r * 2 + 1] = bg
    img = img.resize((COLS * 3, ROWS * 2 * 3), Image.NEAREST)
    img.save(name)
    print("wrote", name)

csi_re = re.compile(rb"\x1b\[([0-9;:<=>?]*)([\x40-\x7e])")

def decode(upto, name):
    grid = [[(" ", (0, 0, 0), (0, 0, 0))] * COLS for _ in range(ROWS)]
    grid = [list(row) for row in grid]
    row = col = 0
    fg = bg = (255, 255, 255)
    i = 0
    while i < upto:
        b = data[i]
        if b == 0x1B and i + 1 < upto and data[i + 1] == 0x5B:
            m = csi_re.match(data, i)
            if not m:
                i += 1
                continue
            params, final = m.group(1).decode(), m.group(2)
            if final == b"H":
                parts = params.split(";") if params else []
                row = int(parts[0]) - 1 if parts and parts[0] else 0
                col = int(parts[1]) - 1 if len(parts) > 1 and parts[1] else 0
            elif final == b"m":
                nums = [int(x) if x else 0 for x in re.split("[;:]", params)] if params else [0]
                j = 0
                while j < len(nums):
                    n = nums[j]
                    if n == 0:
                        fg = bg = (255, 255, 255)
                    elif n == 38 and j + 4 < len(nums) and nums[j + 1] == 2:
                        fg = tuple(nums[j + 2:j + 5]); j += 4
                    elif n == 48 and j + 4 < len(nums) and nums[j + 1] == 2:
                        bg = tuple(nums[j + 2:j + 5]); j += 4
                    j += 1
            elif final == b"J":
                grid = [[(" ", (0, 0, 0), (0, 0, 0))] * COLS for _ in range(ROWS)]
                grid = [list(row_) for row_ in grid]
            i = m.end()
            continue
        if b == 0xE2 and data[i:i + 3] == b"\xe2\x96\x80":
            if 0 <= row < ROWS and 0 <= col < COLS:
                grid[row][col] = ("▀", fg, bg)
            col += 1
            i += 3
            continue
        if b == 0x20:
            if 0 <= row < ROWS and 0 <= col < COLS:
                grid[row][col] = (" ", fg, bg)
            col += 1
            i += 1
            continue
        i += 1
    rasterize(grid, name)

for point in POINTS:
    decode(int(len(data) * point), f"frame_{int(point*100):03d}.png")
