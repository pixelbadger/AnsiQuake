#!/bin/sh
# Fetch the freely redistributable Quake 1.06 shareware data (episode 1)
# and install id1/pak0.pak next to the game binary.
set -e

MIRROR="https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake/quake106.zip"
BASEDIR="$(dirname "$0")/.."
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

command -v lhasa >/dev/null 2>&1 || {
    echo "error: 'lhasa' is required to unpack resource.1 (apt install lhasa)" >&2
    exit 1
}

echo "Downloading Quake 1.06 shareware..."
curl -sfL -o "$WORKDIR/quake106.zip" "$MIRROR"
unzip -o -q "$WORKDIR/quake106.zip" -d "$WORKDIR"
(cd "$WORKDIR" && lhasa xq resource.1 id1/pak0.pak)

mkdir -p "$BASEDIR/id1"
cp "$WORKDIR/id1/pak0.pak" "$BASEDIR/id1/pak0.pak"
chmod 644 "$BASEDIR/id1/pak0.pak"
echo "Installed $(cd "$BASEDIR" && pwd)/id1/pak0.pak"
