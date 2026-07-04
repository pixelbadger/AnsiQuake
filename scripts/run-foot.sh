#!/bin/sh
# Launch tyr-quake interactively in the Foot terminal, sized so the
# terminal's pixel grid (cols x 2*rows) is at or above the software
# renderer's minimum internal resolution (320x200 -- MINWIDTH/MINHEIGHT in
# include/r_shared.h). Below that floor, common/vid_term.c is forced to
# render at 320x200 and point-sample *down* to the smaller physical grid,
# which garbles HUD/console text. At or above the floor it maps 1:1 with
# no scaling -- so "launch big enough" is the whole fix.
#
# Override via env: FOOT_COLS, FOOT_ROWS, FOOT_PIXELSIZE. Extra args are
# passed through to tyr-quake, e.g. `scripts/run-foot.sh +timedemo demo1`.
set -e

BASEDIR="$(cd "$(dirname "$0")/.." && pwd)"
COLS="${FOOT_COLS:-340}"
ROWS="${FOOT_ROWS:-110}"
PIXELSIZE="${FOOT_PIXELSIZE:-4}"

if [ "$COLS" -lt 320 ] || [ "$((ROWS * 2))" -lt 200 ]; then
    echo "error: FOOT_COLS=$COLS FOOT_ROWS=$ROWS gives a ${COLS}x$((ROWS * 2)) pixel grid," >&2
    echo "       below the 320x200 render floor -- text will garble. Need COLS>=320, ROWS>=100." >&2
    exit 1
fi

exec foot -f "monospace:pixelsize=$PIXELSIZE" -W "${COLS}x${ROWS}" \
    -e sh -c '"$@"; exec "${SHELL:-sh}"' sh \
    "$BASEDIR/bin/tyr-quake" -basedir "$BASEDIR" "$@"
