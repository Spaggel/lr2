#!/usr/bin/env bash
# Copy the license texts of everything linked into the game, plus
# CREDITS.md, into a player package.
#
#   tools/collect-licenses.sh <package dir> [<llvm-mingw dir> <triple>]
#
# With llvm-mingw (Windows builds) it adds the notices of the MinGW-w64
# runtime and winpthreads, which are linked statically, and LLVM's license.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$1/LICENSES"

rm -rf "$dest"
mkdir -p "$dest"
cp "$root/CREDITS.md" "$1/CREDITS.txt"
cp "$root/LICENSE" "$dest/LR2-native-port.txt"
cp "$root/third_party/SDL3/LICENSE.txt" "$dest/SDL3.txt"
cp "$root/third_party/SDL3/src/hidapi/LICENSE-bsd.txt" "$dest/SDL3-hidapi.txt"
cp "$root/third_party/SDL3/src/video/yuv2rgb/LICENSE" "$dest/SDL3-yuv2rgb.txt"
cp "$root/third_party/zlib/LICENSE" "$dest/zlib.txt"
if [ $# -ge 3 ]; then
    mingw="$2/$3/share/mingw32"
    cp "$mingw/COPYING.MinGW-w64-runtime.txt" "$dest/mingw-w64-runtime.txt"
    cp "$mingw/COPYING.winpthreads.txt" "$dest/winpthreads.txt"
    cp "$2/LICENSE.TXT" "$dest/LLVM.txt"
fi
