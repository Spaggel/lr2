#!/usr/bin/env bash
# Cross-build the Windows version (x64 and ARM64) from Linux or macOS with
# llvm-mingw, as ready-to-run folders dist/LR2-windows-<x64|arm64>/ holding
# lr2.exe, the HD texture pack and README.txt.  No game files: on first
# start the game copies them from the player's CD into game/.
#
#   tools/build-windows.sh [options] [x86_64] [aarch64]
#
#   --zip         also make dist/LR2-windows-<arch>.zip (--package works too)
#   --debug       keep debug info (RelWithDebInfo) instead of Release
#
# With no architecture given, both are built.  Needs cmake, ninja, curl,
# tar and python3.  llvm-mingw is downloaded to tools/local unless
# LLVM_MINGW points at an existing copy.  SDL3 is built from
# third_party/SDL3 as a static library for each architecture.
set -euo pipefail

LLVM_MINGW_VERSION=20260922
# sha256 of the release archive per host (macOS ships bash 3: no
# associative arrays); other hosts' archives are not checked.
llvm_mingw_sha256() {
    case "$1" in
    ubuntu-22.04-x86_64.tar.xz) echo bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21 ;;
    esac
}

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

zip=0 build_type=Release archs=()
for a in "$@"; do
    case "$a" in
    --zip | --package) zip=1 ;;
    --debug) build_type=RelWithDebInfo ;;
    x86_64 | x64 | amd64) archs+=(x86_64) ;;
    aarch64 | arm64) archs+=(aarch64) ;;
    -h | --help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done
[ ${#archs[@]} -gt 0 ] || archs=(x86_64 aarch64)

tool() {
    if command -v "$1" >/dev/null; then command -v "$1"
    elif [ -x "tools/.venv/bin/$1" ]; then echo "$root/tools/.venv/bin/$1"
    else echo "error: $1 not found (install it, or pip install cmake ninja into tools/.venv)" >&2; exit 1
    fi
}
CMAKE="$(tool cmake)"
NINJA="$(tool ninja)"

# ------------------------------------------------------------ llvm-mingw
if [ -z "${LLVM_MINGW:-}" ]; then
    case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) host=ubuntu-22.04-x86_64.tar.xz ;;
    Linux-aarch64) host=ubuntu-22.04-aarch64.tar.xz ;;
    Darwin-*) host=macos-universal.tar.xz ;;
    *) echo "error: no llvm-mingw download for this host; set LLVM_MINGW" >&2; exit 1 ;;
    esac
    name="llvm-mingw-$LLVM_MINGW_VERSION-ucrt-${host%.tar.xz}"
    LLVM_MINGW="$root/tools/local/$name"
    if [ ! -x "$LLVM_MINGW/bin/clang" ]; then
        mkdir -p tools/local/dl
        archive="tools/local/dl/$name.tar.xz"
        url="https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$name.tar.xz"
        echo "== downloading $url"
        curl -fL --retry 3 -o "$archive.part" "$url"
        want="$(llvm_mingw_sha256 "$host")"
        if [ -n "$want" ]; then
            got="$( (sha256sum "$archive.part" 2>/dev/null || shasum -a 256 "$archive.part") | cut -d' ' -f1)"
            [ "$got" = "$want" ] || { echo "error: checksum mismatch for $archive" >&2; exit 1; }
        fi
        mv "$archive.part" "$archive"
        tar -xJf "$archive" -C tools/local
    fi
fi
export LLVM_MINGW
echo "== llvm-mingw: $LLVM_MINGW"

if [ ! -f third_party/SDL3/CMakeLists.txt ] || [ ! -f third_party/zlib/zlib.h ]; then
    git submodule update --init third_party/SDL3 third_party/zlib
fi

# Make dst a copy of src.
sync_dir() {
    if command -v rsync >/dev/null; then
        rsync -a --delete "$1/" "$2/"
    else
        rm -rf "$2" && cp -R "$1" "$2"
    fi
}

# ----------------------------------------------------------------- build
for arch in "${archs[@]}"; do
    toolchain="$root/cmake/toolchains/llvm-mingw-$arch.cmake"
    sdl_build="third_party/build/SDL3-windows-$arch"
    sdl_prefix="$root/tools/local/sdl3-windows-$arch"
    if [ ! -f "$sdl_prefix/lib/cmake/SDL3/SDL3Config.cmake" ]; then
        echo "== SDL3 for windows-$arch"
        "$CMAKE" -S third_party/SDL3 -B "$sdl_build" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
            -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$sdl_prefix" \
            -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
        "$NINJA" -C "$sdl_build" install
    fi

    echo "== lr2.exe for windows-$arch"
    build="build/windows-$arch"
    "$CMAKE" -S . -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE="$build_type" \
        -DSDL3_DIR="$sdl_prefix/lib/cmake/SDL3"
    "$NINJA" -C "$build"

    case "$arch" in x86_64) label=x64 ;; aarch64) label=arm64 ;; esac
    dist="dist/LR2-windows-$label"
    echo "== $dist"
    mkdir -p "$dist"
    cp "$build/lr2.exe" "$dist/"
    cp tools/player-readme.txt "$dist/README.txt"
    bash tools/collect-licenses.sh "$dist" "$LLVM_MINGW" "$arch-w64-mingw32"
    # The HD textures always ship with the game.  The copies are large, so
    # they are only refreshed when something changed.
    [ -d "HD TEXTURES" ] || { echo "error: HD TEXTURES folder missing" >&2; exit 1; }
    sync_dir "HD TEXTURES" "$dist/HD TEXTURES"
    if [ $zip = 1 ]; then
        rm -f "$dist.zip"
        (cd dist && python3 -I -m zipfile -c "LR2-windows-$label.zip" "LR2-windows-$label")
        echo "   $(du -sh "$dist.zip" | cut -f1)  $dist.zip"
    fi
done
