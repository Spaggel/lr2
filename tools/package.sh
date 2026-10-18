#!/usr/bin/env bash
# Build the player package for this machine's OS: the recompiled game and
# the HD texture pack, without any game files (on first start the game asks
# for the player's own CD and copies its files into game/ next to itself).
#
#   macOS: dist/LR2-macos.zip               LEGO Racers 2.app, arm64 + x86_64
#   Linux: dist/LR2-linux-<arch>.tar.gz     lr2, HD TEXTURES/
#
# Windows packages come from tools/build-windows.sh.  Needs cmake, ninja and
# a C compiler (on Linux also SDL3's build dependencies, see README.md).
# SDL3 is built from third_party/SDL3 and linked statically.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

case "$(uname -s)" in
Darwin)
    label=macos
    # Universal binary; macOS 11 is the first with Apple Silicon.
    arch_flags=(-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0)
    ;;
Linux)
    label="linux-$(uname -m)"
    arch_flags=()
    ;;
*) echo "error: use tools/build-windows.sh for Windows" >&2; exit 1 ;;
esac

[ -d "HD TEXTURES" ] || { echo "error: HD TEXTURES folder missing" >&2; exit 1; }
if [ ! -f third_party/SDL3/CMakeLists.txt ] || [ ! -f third_party/zlib/zlib.h ]; then
    git submodule update --init third_party/SDL3 third_party/zlib
fi

sdl_build="third_party/build/SDL3-$label"
sdl_prefix="$root/tools/local/sdl3-$label"
if [ ! -f "$sdl_prefix/lib/cmake/SDL3/SDL3Config.cmake" ]; then
    echo "== SDL3 for $label"
    cmake -S third_party/SDL3 -B "$sdl_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$sdl_prefix" -DCMAKE_INSTALL_LIBDIR=lib "${arch_flags[@]}" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    ninja -C "$sdl_build" install
fi

echo "== lr2 for $label"
build="build/release-$label"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSDL3_DIR="$sdl_prefix/lib/cmake/SDL3" "${arch_flags[@]}"
ninja -C "$build"

dist="dist/LR2-$label"
echo "== $dist"
rm -rf "$dist"
mkdir -p "$dist"
cp tools/player-readme.txt "$dist/README.txt"
bash tools/collect-licenses.sh "$dist"
if [ "$label" = macos ]; then
    app="$dist/LEGO Racers 2.app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$build/lr2" "$app/Contents/MacOS/"
    cp runtime/res/lr2.icns "$app/Contents/Resources/"
    cp -R "HD TEXTURES" "$app/Contents/Resources/"
    cat >"$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>lr2</string>
    <key>CFBundleIdentifier</key><string>io.github.lr2native</string>
    <key>CFBundleName</key><string>LEGO Racers 2</string>
    <key>CFBundleIconFile</key><string>lr2</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$(git describe --tags --always)</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
    # Ad-hoc signature: Apple Silicon refuses unsigned code.
    codesign --force --deep --sign - "$app"
    (cd dist && rm -f "LR2-$label.zip" && ditto -c -k --keepParent "LR2-$label" "LR2-$label.zip")
    out="dist/LR2-$label.zip"
else
    cp "$build/lr2" "$dist/"
    cp -R "HD TEXTURES" "$dist/"
    tar -C dist -czf "dist/LR2-$label.tar.gz" "LR2-$label"
    out="dist/LR2-$label.tar.gz"
fi
echo "   $(du -sh "$out" | cut -f1)  $out"
