# Credits and acknowledgements

This project's own code is MIT-licensed (see `LICENSE`); everything below
keeps its own terms.

## The game

LEGO Racers 2 (2001) was developed by Attention to Detail and published by
LEGO Software. LEGO and the LEGO logo are trademarks of the LEGO Group. This
project is not affiliated with, sponsored or endorsed by the LEGO Group or
Attention to Detail.

This repository and the player packages contain **none of the game's
files**: no data archive, levels, cars, movies, music or original
programs. Players supply their own CD (or a disc image made from it); on
first start the game copies the files it needs from there.

What is here and comes from the original game:

- `build/gen/`: the game's code, translated to C by the static recompiler in
  `tools/recomp`. The original `LEGO Racers 2.exe` it is made from is not in
  the repository; to regenerate, take it from your own CD (see README.md).
- `build/dis.txt`, `build/ghidra/`, `build/ghidra_functions.txt`: analysis
  of the original program, used by the recompiler.
- `runtime/res/lr2.ico`, `runtime/res/lr2.icns`: the game's own icon.

## HD texture pack

`HD TEXTURES/` is **"Higher resolution textures (upscaled, less
blurry/noisy)" by dorpz**, published on Rock Raiders United:
<https://rockraidersunited.com/files/lego-racers-2/higher-resolution-textures-upscaled-less-blurrynoisy-r335/>.
Many thanks to dorpz for making it freely available.

## Libraries in the game

Linked into every build (`lr2`, `lr2.exe`, the macOS app):

| Library | Authors | License |
|---|---|---|
| [SDL3](https://www.libsdl.org/) (`third_party/SDL3`) | Sam Lantinga and the SDL contributors | zlib |
| ↳ HIDAPI, bundled in SDL (gamepads) | Alan Ott, Signal 11 Software | BSD-3-Clause (one of three options) |
| ↳ yuv2rgb, bundled in SDL | Adrien Descamps | BSD-3-Clause |
| ↳ stb_image, bundled in SDL | Sean Barrett and contributors | Public domain / MIT |
| [zlib](https://zlib.net/) (`third_party/zlib`) | Jean-loup Gailly and Mark Adler | zlib |

Windows builds also statically link parts of the toolchain's runtime:

| Library | Authors | License |
|---|---|---|
| [mingw-w64](https://www.mingw-w64.org/) runtime | the mingw-w64 project and contributors | ZPL-2.1 and others (see its notice) |
| winpthreads | the mingw-w64 project; parts by Lockless Inc. | MIT and BSD-3-Clause |
| LLVM compiler-rt builtins | the LLVM project | Apache-2.0 WITH LLVM-exception |

The player packages carry the full license texts in a `LICENSES` folder
(collected by `tools/collect-licenses.sh`).

## Tools used to build it

Not shipped to players, but the project would not exist without them:

- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) by Martin Storsjö:
  the toolchain for the Windows builds (LLVM: Apache-2.0 WITH
  LLVM-exception).
- [Capstone](https://www.capstone-engine.org/) by Nguyen Anh Quynh and
  contributors (BSD-3-Clause): the disassembler used by the recompiler.
- [pefile](https://github.com/erocarrera/pefile) by Ero Carrera (MIT):
  reading the original program and its icon.
- [Ghidra](https://ghidra-sre.org/) by the National Security Agency
  (Apache-2.0): function analysis of the original program
  (`tools/ghidra/DumpFunctions.java`).
- [CMake](https://cmake.org/) by Kitware (BSD-3-Clause), [Ninja](https://ninja-build.org/)
  (Apache-2.0) and [Python](https://www.python.org/) (PSF License).

The repository also lists [unshield](https://github.com/twogood/unshield)
(MIT) and [libXext](https://gitlab.freedesktop.org/xorg/lib/libxext) (X11/MIT)
as submodules; the current build does not use them.
