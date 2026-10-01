#!/bin/sh
#
# Cicada.icns is the *rounded-white* macOS icon.  Pillow cannot write a real
# macOS icon family, so the .iconset directory next to this script is the
# source of truth and this wrapper feeds it to Apple's own tool.
#
# MUST BE RUN ON macOS (iconutil ships with Xcode / the command line tools):
#
#     cd platform/QtPlayer/assets/appicon
#     ./make_icns.sh
#
# It reads  ./Cicada.iconset   and writes  ./Cicada.icns
# (both paths are resolved relative to this script, so any cwd works)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
iconset="$here/Cicada.iconset"
target="$here/Cicada.icns"

if [ ! -d "$iconset" ]; then
    echo "make_icns.sh: $iconset is missing -- run" >&2
    echo "    python tools/make_icons/make_icons.py" >&2
    echo "first (it generates the .iconset PNGs)." >&2
    exit 1
fi

if ! command -v iconutil >/dev/null 2>&1; then
    echo "make_icns.sh: iconutil not found -- this script only runs on macOS" >&2
    exit 1
fi

# -c icns asks for the modern (macOS 10.7+) icon family; -o is the output file.
iconutil -c icns "$iconset" -o "$target"
echo "wrote $target"
