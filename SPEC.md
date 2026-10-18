# LEGO Racers 2 native port: spec and status

Last updated 2026-10-18. This file is the hand-off document between sessions:
read it first, then keep it current.

## Goal

The user owns LEGO Racers 2 for PC (`LEGO Racers 2.iso`, extracted into
`game/cd`) and wants:

- A **true native ARM64 macOS port**. They explicitly chose this over Wine or
  other compatibility layers.
- Modern controller support.
- Savestates.
- Portable builds for Windows 10/11 (x64 and ARM64): **done**, see
  "Windows build" below. Possibly later WASM in the browser if controllers
  work there (the Gamepad API does). Keep the code portable.

Development and testing happen in WSL2 Linux (llvmpipe GL, dummy or real
audio). The user builds on an Apple Silicon MacBook (no DVD drive) from a
git clone; the macOS build works there.

**Repository:** private GitHub repo `git@github.com:Spaggel/lr2.git`
(`origin`, branch `master`). It holds the recompiler, the runtime, the
generated `build/gen`, `HD TEXTURES/` (by dorpz, see `CREDITS.md`) and the
Ghidra project, but no game files and no original executables: `game/` and
`orig/` are ignored. Commit only when the user asks.

- The repository must stay shareable: never commit the CD's contents
  (`GAMEDATA.GTC`, movies, music, installer, redistributables) or any
  original .exe. They were tracked in early commits; on 2026-10-18 the
  whole history was rewritten (git filter-repo) to remove `game/` and
  `orig/LEGORacers2.exe` from every commit.
- To regenerate `build/gen`, copy `install/LEGO Racers 2.exe` from the CD
  to `orig/LEGORacers2.exe` (ignored).
- The third-party libraries in `third_party/` are submodules. `game/save`
  and build output are ignored.

## Approach

The original x86 exe is statically recompiled into C:
`orig/LEGORacers2.exe`, sha256 `074899f5…`, 32-bit MSVC, D3D8, DInput8 and
DSound, pure x87 FPU with no SSE or MMX, no DRM.

A runtime replaces Win32, D3D8, DInput8 and DSound with SDL3 and OpenGL 3.3
core.

- `tools/recomp/` (Python, capstone): `recomp.py` takes the exe and produces
  `build/gen/*.c` plus `gen_files.cmake`. The modules are `analysis.py`,
  `codegen.py`, `x87.py` and `overrides.py`.
- `runtime/`: the C runtime. `CMakeLists.txt` globs `runtime/src/*.c` and
  links SDL3.
- Build: `tools/.venv/bin/ninja -C build/native`. cmake and ninja live in
  `tools/.venv/bin`, not on PATH. The binary is `build/native/lr2`.
- Run:
  `build/native/lr2 --game game/cd/install --cd game/cd --save game/save --log 2`.
  The game streams its data from `GameData.gtc` itself, so nothing needs
  extracting.

### Recompiled code model

- Every guest function becomes `void f_XXXXXXXX(CPU *restrict c)`. Guest
  memory is a flat 4 GiB mmap at `g_mem`. The guest registers live in `CPU`.
  Flags are function-local.
- `call` pushes the real guest return address and calls the C function
  directly, so **the guest stack and the host C stack mirror each other**.
  `ret n` is `c->esp += 4+n; return;`.
- Imports and COM methods are host functions with the same convention:
  - `ARG(n)` reads arguments; for COM, `ARG(0)` is `this`.
  - `RET(nargs, v)` pops the return address and `4*nargs` bytes; nargs
    includes `this`.
- Host functions are reached through thunks at `0x800000` (16 bytes each).
  `thunk_register(fn, name)` creates one; `call_indirect` → `thunk_lookup`.
- `guest_call(c, addr, nargs, args)` calls guest code from the host.
- COM objects in guest memory are `{vtbl, id}`, where `id` indexes the host
  table in `win32/com.c`. Helpers: `com_vtbl`, `com_wrap`, `com_get`,
  `COM_THIS(T)`, `com_addref_obj`, `com_release_obj`.

### Guest memory layout (`rt.h`)

| Region | Range |
|---|---|
| PEB | `0x10000` |
| Environment | `0x20000` |
| Main stack | `0x100000`–`0x300000` |
| Image | `0x400000` |
| Thunks | `0x800000` |
| Thread stacks | `0x1000000`, 1 MiB slots |
| VirtualAlloc arena | `0x4000000`–`0x20000000` |
| Heap | `0x20000000`–`0x70000000` |

The heap is a boundary-tag allocator whose metadata lives in guest memory.
`gmalloc`, `gcalloc` and `gfree` hand out guest addresses.

### Game facts learned

- The main loop is `f_004c3040` (WinMain). It calls `PeekMessageA` directly
  every frame, and its host call chain is identical in the menu and in races.
- Window class "Saracen II".
- Input is polled on a 28 ms `timeSetEvent` thread (proc `0x4b95b0`) under
  the "ATDControlInterfacePollMutex" named mutex.
- Exactly one guest thread exists: the music streamer, `CreateThread`
  start `0x4f1320`, which calls the loop `f_004f1340`.
  - It loops on `MsgWaitForMultipleObjects(1, …, INFINITE, 0xbf)` from
    `0x4f1375`, then fills the stream through `f_4f19d0`.
  - Music files are `game data\music\*.N`: MS-ADPCM WAV, stereo, 22050 Hz,
    decoded by the game into a 2-channel 22050 Hz streaming DSound buffer
    with flags `0x10180`.
- SFX buffers are mono, 16-bit, 22050 Hz, flags `0xe0`. The game computes
  engine volume, pan and pitch itself; DS3D is not used for engines.
- At race start about 6 opponents play the same idle-engine loop at the same
  pitch, all started together. The in-phase sum gives a mix RMS of about 0.4,
  which is faithful behaviour, so a master limiter was added.
- **Data archive.** All game data except music comes from `GameData.gtc`;
  the game never opens loose data files (verified: hiding entries from the
  file list leaves textures missing).
  - `FileList.Inf`: 136-byte records, a NUL-terminated name in bytes 0..127
    (`GAME DATA\COMMON\TEXTURES\PIN_BLUE.MIP`), then u32 offset and u32
    size in an uncompressed address space cut into 128 KiB chunks. Files are
    2048-aligned and not sorted. Count = file size / 136.
  - `Compress.Inf`: one 20-byte record per chunk, `{gtc offset, packed size,
    allocated size, method, files touching the chunk}`. The reader
    (`0x4a4930`) reads `allocated` bytes and decompresses (`0x4e7bb0`)
    unless `allocated >= 0x20000`, in which case it reads 0x20000 bytes as
    is (stored, method 0). Count = file size / 20.
  - Original `.MIP` header (18 bytes): u32 flags, u32, u32, u16 w, u16 h,
    u8 bpp, u8; original textures are 8-bit palettised with mips. The HD
    pack's MIPs are flags `0x20000`, 32 bpp, one level, and the game accepts
    them.
- **Controls** (all addresses in the original exe):
  - A controller (0x17c bytes) holds up to 14 bindings of 6 dwords from
    +0x18, count at +0x14, added by `0x4b7da0` (thiscall, ecx =
    controller). A binding is `{type, index, device kind, device number,
    input kind, code}`: type 0 = axis `index` (0 x, 1 y), 1/2 = negative /
    positive half of that axis from a digital input, 3/4 = action bit
    `index`; device kind 1 keyboard, 2 mouse, 3 joystick; input kind 0
    thresholded axis, 1/2 axis halves, 3 analog axis, 4/5 analog halves,
    6/7 button. `0x4b86f0` evaluates them each poll. Action bits are set
    *or cleared* per binding, so two bindings for one action fight; and an
    axis with any digital binding is rewritten from its eased value at the
    end, so analog and digital bindings cannot share an axis.
  - Joystick device class (vtable `0x536e70`): `+8` button(code) reads
    `rgbButtons[code]` of a `c_dfDIJoystick` state at +0x20c4 if code <
    `DIDEVCAPS.dwButtons` (+0xa0); `+0xc` axis(code) reads one of exactly 6
    axes (X Y Z Rx Ry Rz = 0..5, count fixed at `0x4ed950`), range set to
    -255..255 and mapped to -1..1. The input manager is `[0x549e80]`;
    vtable +0x10 `(n, 3)` returns joystick n. The game creates joysticks
    only at startup, one per device `EnumDevices` lists.
  - Race actions: 0 accelerate, 1 brake, 2 reverse, 3 fire weapon, 4 brick
    boost, 5 talk, 6 look behind, 7 next target, 8 previous target, 9
    pause. Names are in `GAME DATA\TEXT\LEGO STRINGS.BRI` (UTF-16 strings,
    entry table `{char key[12], u32 rel}` from 0x14c).
  - Per-player control config: 43 dwords at `[0x53e3bc] + 0xb4 + n*0xac`.
    [0] joystick number, [1] kind (1 keyboard, 3 joystick), [2] analog,
    [3..6] keyboard steer left/right as (kind, code), keyboard actions 0-8
    as (kind, code) from [7], joystick actions from [0x19]; kind 1/2 =
    axis half, otherwise a button/key code. Defaults: `0x40a000`. The race
    binds each human player from it with `0x40ad60` (thiscall, ecx =
    config, arg = controller), joystick pause hard-coded as button 8 and
    keyboard pause as Esc.
  - Menus (`0x42af70`) bind joystick 0 and 1 and the keyboard to separate
    frontend controllers: joystick X/Y axes, then actions 0-8 = buttons
    from the table at `0x53060c` (`{0,4,5,2,6,7,8,1,3}`); keyboard arrows,
    then actions 0-6 = Enter, Esc, numpad 2/8/4/6, Esc. Action 0 confirms,
    1 goes back, 6 is the pause/escape button (`0x42b370` returns it).
- Movies: `CoCreateInstance(CLSID_FilterGraph)` returns
  `REGDB_E_CLASSNOTREG`, so the game skips them. DirectMusic has not been
  seen being called.

## Status

### Working (in WSL)

- Boot and main menu.
- Menu navigation and the full path to a race: Arcade → Sandy Bay → race,
  with 3D rendering, HUD and minimap.
- **D3D8 → GL 3.3** (`runtime/src/d3d8/`):
  - Fixed-function emulation in `ffp.c`.
  - Renders to an FBO stored top-down. `gl_Position.y` is flipped; the RHW
    path maps `y` as `1 - (y-vp.y)*2/h`.
  - Lockable resource data lives in guest memory; GL objects are caches
    marked dirty.
- **Keyboard play works:** in a race, Up accelerates, Down brakes, Left/Right
  steer (verified with held keys). The game uses its own DInput keyboard
  device.
- **DInput8** (`input/dinput8.c`): keyboard (event-fed `s_keys`), mouse, and
  up to 4 SDL gamepads.
  - Gamepad layout: X/Y = left stick, Z = LT, Rx/Ry = right stick, Rz = RT,
    POV = d-pad.
  - Buttons 0–9 are A B X Y LB RB Back Start LS RS; 10/11 are LT/RT as
    digital buttons; 12 is Guide.
  - Supports buffered data, properties, and enumeration.
- **DSound8 + DS3D** (`audio/dsound.c`): software mixer to SDL F32 stereo at
  44.1 kHz with linear interpolation and volume/pan/frequency, plus
  notifications, duplicates and a **master peak limiter** (`limit()`, ceiling
  0.95, ~100 ms release).
- Named events and mutexes, `OpenMutexA`, and a timer thread that skips
  missed ticks.
- **HD texture pack** (`hdtex.c`): 2,180 `.MIP` files in `HD TEXTURES/game
  data/...`, used by default (`--hd DIR`, `--no-hd`; auto-detected in the
  game dir, CD dir or cwd).
  - `hd_init` reads the original `FileList.Inf`/`Compress.Inf`, matches pack
    files by upper-cased name, places them 2048-aligned in a region after
    the last original chunk, and appends stored chunk records whose gtc
    offsets start at the real `.gtc` size (rounded to 2048).
  - `hd_open` (used by `CreateFileA` and the savestate reopen) serves the
    patched index files from `os_tmpfd()`, recognised by `os_path_id`
    (dev/inode; volume serial/file index on Windows).
  - `ReadFile` calls `hd_fill` when a read of `GameData.gtc` comes up short:
    bytes past the real end come from the pack files (opened per read).
    `GetFileSize` reports the extended size.
  - Section `HDTX` stores the pack's file count and a hash, and its `check`
    refuses states saved with a different pack or with HD on/off switched.
    States from before this section existed no longer load.
  - Verified: menus, character builder and a Sandy Bay race render with the
    HD textures; no measurable slowdown; save/load works with the pack.
  - Possible improvement: the HD textures have one mip level, so distant
    ground shimmers a little; generating GL mipmaps for single-level
    textures would fix that, at the cost of differing from D3D behaviour.

### Gamepad controls (`input/lr2pad.c`, `input/dinput8.c`)

- Out of the box the pad worked only in menus: player configs default to
  the keyboard, so races never bound the joystick.
- DirectInput gamepad layout: buttons A B X Y LB RB Back LS **Start(8)** RS
  LT RT Guide, then virtual buttons 13-16 = right stick down/up/left/right
  and 17-26 = race actions 0-9; the Ry axis slot is a virtual steering axis
  (left stick with a 0.15 dead zone, plus d-pad and the player's steer keys
  eased in over 1/6 s and out over 1/8 s). The d-pad is also merged into
  the X/Y axes for menus. A race action button is the pad's button for it
  OR the player's key from the config (Esc for pause), so the keyboard
  keeps working.
- `tools/recomp/overrides.py` `WRAPS` renames a recompiled function to
  `<name>_orig` and routes calls through a runtime function.
  `lr2pad_race_bindings` wraps `0x40ad60`: for player n < 2 with a keyboard
  config and gamepad slot n present (and the game's joystick reporting all
  27 buttons, which states from older builds do not), it binds the steering
  axis (analog) and the 10 virtual action buttons instead; otherwise the
  original runs. A joystick config chosen in the options keeps the game's
  own bindings.
- **Native resolution / widescreen** (after alphayellow1's
  LegoRacers2WidescreenFix): `overrides.py` `PATCHES` replaces single
  instructions with C statements. The default mode before `LR2.cfg` exists
  (`0x49b5d0`) becomes the desktop's native pixel size at 32-bit; the
  engine's mode whitelist (`0x42a911`) accepts native instead of 800x600;
  the camera's horizontal FOV (`0x4ba425`, width > height branch) goes
  through `lr2_hor_fov` for Hor+ on screens wider than 4:3. `d3d8.c`
  offers the native size (in pixels, also on high-DPI macOS) as the last
  adapter mode. 2D (HUD, menus) is still laid out for 4:3 and stretches.
- Race layout: RT accelerate, LT reverse, B brake, A fire, X brick boost,
  Y talk, LB / RS click look behind, RB / d-pad down next target, d-pad up
  previous target, Start pause. Menus: `lr2pad_init` (after `pe_load`)
  rewrites the menu table to A, B, RS down/up/left/right, Start, X, Y.
- Savestates: pad devices from states saved before the virtual buttons
  existed load with the new buttons at their standard format offsets.
- Not run yet (the user asked not to start the game until they say so);
  test with `LR2_AUTOINPUT` `pad:` steps and on a real pad.

### Player packages and first-run setup

- The game is always the recompiled build: `recomp.py` also writes the
  exe's bytes to `build/gen/gen_image.c` (`g_image`), and `pe_load` maps
  the data sections from there. No .exe is read at run time, and setup
  does not copy any.
- `setup.c`: if no game data is found (or with `--setup [ISO|DIR]`), a
  window asks the player to drag in their disc image, CD or a copy of it
  (clicking opens a file dialog; an `.iso` next to the program is used
  without asking). It copies the CD layout into `<program dir>/game/`,
  skipping `DCOM95/`, `eReg/`, `Manual/`, `install/redist/` and `*.exe`;
  the game needs `install/` and also `language/` (it looks for
  `D:\language\export.<lang>` and asks for the CD without it) and `ld/`.
  ISO images are read through their Joliet tree (ISO9660 names as the
  fallback). `GAMEDATA.GTC` is copied last via `.partial` + rename, so its
  presence marks a complete copy. A macOS app run translocated (downloaded,
  not moved) gets a message to move it instead.
- `tools/package.sh` (macOS: universal `.app`, ad-hoc signed, HD pack in
  `Contents/Resources`, which is `SDL_GetBasePath()`; Linux: tarball) and
  `tools/build-windows.sh` build static SDL3 from `third_party/SDL3`.
  `.github/workflows/release.yml` runs them on GitHub (macos-14,
  ubuntu-22.04 for Linux x86-64 and the Windows cross-build) and publishes
  a release for tags `v*`.
- Tested on macOS (arm64): package build, setup from an ISO made with
  `hdiutil makehybrid -iso -joliet`, race with HD textures. Linux (arm64,
  Ubuntu 22.04 container) and Windows x64 packages build; not run yet.

### Windows build (native, x64 and ARM64)

- Cross-built from Linux/macOS with llvm-mingw (pinned 20260922, UCRT):
  `tools/build-windows.sh [--zip] [--debug] [x86_64]
  [aarch64]`. It downloads llvm-mingw to `tools/local/` (sha256-checked on
  Linux x86-64), builds static SDL3 from `third_party/SDL3` per arch into
  `tools/local/sdl3-windows-<arch>`, then `build/windows-<arch>/lr2.exe`.
  Toolchain files: `cmake/toolchains/llvm-mingw-{x86_64,aarch64}.cmake`.
  It always makes `dist/LR2-windows-{x64,arm64}/` with
  `tools/player-readme.txt` as README.txt and the `HD TEXTURES` pack
  (always; the script fails without it; synced with rsync when available),
  no game files; `--zip` (alias `--package`) zips each folder too.
- Exit crash (fixed): the game stops its music thread by posting WM_QUIT
  to the thread id `CreateThread` returned. That id used to be the handle
  value and was registered only once the thread ran, so the quit message
  could be lost and shutdown crashed. Thread ids are now allocated in
  `CreateThread` before the thread starts (`sync.c`). The `close` autoinput
  step exercises this path (`SDL_EVENT_QUIT`, like closing the window).
- The exe is a GUI-subsystem program (SDL_main.h supplies WinMain), linked
  `-static` (SDL3, winpthreads, compiler-rt): imports only Windows system
  DLLs and the UCRT. 16 MiB main stack (`--stack`), `--export-all-symbols`
  so `dladdr()` can name functions from the export table (6,617 `f_`
  exports, well under the 65,535 limit). Resources in `runtime/res/`: the
  game's own icon (`tools/extract_icon.py`), a manifest (UTF-8 active code
  page, PerMonitorV2 DPI, long paths, Win10 supportedOS) and version info.
- Platform layer: `runtime/src/os.h` with `os/os_posix.c` and
  `os/os_win32.c` (each compiles to nothing on the other platform; the
  Win32 one must not include `rt.h`, whose names clash with `windows.h`):
  - `os_reserve`: Windows reserves the 4 GiB + 64 KiB guest space and a
    vectored exception handler commits 1 MiB blocks on first touch (the
    first 64 KiB stay a guard). Kernel I/O into never-touched pages fails
    with ERROR_NOACCESS instead of faulting, so `ReadFile`/`WriteFile`
    call `os_commit` on the guest buffer first.
  - `os_on_fault`: crash report via sigaction / unhandled exception filter.
  - `os_fd_id`/`os_path_id`, `os_tmpfd` (UCRT `tmpfile()` writes to the
    drive root; this uses `%TEMP%` + FILE_FLAG_DELETE_ON_CLOSE),
    `os_replace` (rename over an existing file), `os_log_file` (stderr to
    `<save>/lr2.log` when there is no console), `os_init` (binary
    `_fmode`, `timeBeginPeriod(1)` so Sleep/usleep/condvar timeouts are
    1 ms, no error dialogs).
  - Shim headers in `runtime/compat/win32/` (only on the WIN32 include
    path): `execinfo.h` (RtlCaptureStackBackTrace), `dlfcn.h` (dladdr from
    the export table), `fnmatch.h`, `unistd.h` (+pread/pwrite via
    OVERLAPPED, file position restored), `sys/stat.h` (2-arg `mkdir`).
  - SIGUSR1 (main-thread dump) and the SIGUSR2 "did not park" backtrace are
    POSIX-only. File handles store their open flags (`HObj.oflags`) instead
    of `fcntl(F_GETFL)`.
- Default directories (all platforms, without `--game`): `GAMEDATA.GTC`
  is looked for in the cwd and the program's dir (the `.app`'s dir on
  macOS), directly or under `game/install` (package layout: `game/` = CD)
  or `game/cd/install` (this repo). The CD dir is then the parent of
  `install`, saves go to `<root>/game/save`.
  So `build/native/lr2` run from the repo root needs no options now. The HD
  pack is also looked for next to the exe. `--windowed` = `LR2_WINDOWED`.
  `rt_fatal` also shows an SDL message box.
- Verified on Windows 10 x64 (RX 6800 XT, run from WSL via interop in
  `C:\Users\Daniel\AppData\Local\Temp\lr2-wintest`): boot, race with HD
  textures, savestate save and load in a new process (call chains match),
  log file when started without a console, game found from another cwd.
  ARM64: builds and links (system DLLs only) but has not been run; Windows
  on ARM needs the OpenGL compatibility pack (GLOn12) on Snapdragon.
- Savestates are not portable between platforms/compilers (call chains
  hold function offsets and open flags differ).

### Debug environment variables

| Variable | Effect |
|---|---|
| `LR2_AUTOINPUT="600:Return,700:Down,…,900:shot,1000:bt,2000:quit"` | Scripted keys by frame (SDL scancode names, held 4 frames, or N frames with `Up/N`). `shot` dumps a frame, `bt` logs the host call chain at Present and the next PeekMessageA, `save:S` / `load:S` use savestate slot S, `quit` exits, `close` closes the window the normal way. `pad:NAME[=V][/N]` drives a virtual SDL gamepad (A B X Y LB RB Back Start Guide LS RS Up Down Left Right, axes LX LY RX RY LT RT with V in -1..1). |
| `LR2_VIRTUAL_PAD` | With `LR2_AUTOINPUT`, attach the virtual gamepad even without `pad:` steps. |
| `LR2_DUMP_FRAMES=N` | Write every Nth frame as a BMP to `<save>/frames/`. |
| `LR2_TRACE_DRAWS=N` | Log draw calls with render state. |
| `LR2_TRACE_THUNKS` | Every 300 frames, log host-call counts. |
| `LR2_FFP_DEBUG=2..6` | Shader debug views. |
| `LR2_DUMP_SHADERS` | Log generated shader source. |
| `LR2_AUDIO_DUMP=file` | Write the raw float32 stereo mix to a file. |
| `LR2_AUDIO_STATS` | Every 2 s, log mix RMS, clip count and per-buffer gain/RMS. |
| `LR2_WINDOWED` | Run windowed. |

Script to reach a race (Arcade → Sandy Bay → Dig-a-Brick) with a fresh
save directory:

```
LR2_AUTOINPUT="700:Return/15,1000:Down/15,1100:Return/15,1400:Return/15,1600:Return/15,1800:Return/15,2000:Return/15,2200:Return/15,2400:Return/15,2600:Return/15,2800:Return/15,4600:quit"
```

The race is running by frame 3200. Keys held for the default 4 frames are
often missed by the game's 28 ms input poll; hold them for 15. `quit` waits
for pending savestate writes (`snap_wait_writers`) before `_exit`. The scratchpad tool `strip.py` builds a
contact sheet of dumped frames; the Python venv is `tools/.venv` (Pillow,
numpy, soundfile).

## Savestates: WORKING

### Design (see `runtime/src/savestate.h`)

Because host frames mirror guest frames, a snapshot is taken and restored
only while every thread is parked at a known safe point:

- **Main thread:** `snap_safepoint(c)` at the top of `PeekMessageA`. Requests
  are queued by hotkeys and run there.
- **Guest threads:** parked inside `wait_handles()` (via `snap_park()`). The
  wait imports keep their args on the guest stack, so after a load
  `wait_handles` returns `WAIT_RESTART` and the import re-reads its args from
  the restored stack. This applies to `WaitForSingleObject`,
  `WaitForMultipleObjects` and `MsgWaitForMultipleObjects`.
- **Sleep:** also a park point.
- **Timer threads (winmm):** park between callbacks, then re-read their
  settings.
- **Critical sections:** a thread never parks while it holds one
  (`thread_cs_held()` in `sync.c` counts Enter/Leave per thread), and the
  main safepoint defers a pending request to a frame where it holds none.
  Otherwise a parked thread can hold a guest lock that the timer callback
  needs, and the world stop times out. Because no lock is ever held in a
  snapshot, the host mutexes behind critical sections never need saving.

**World stop.** `g_snap_stop` is set, the wakers broadcast the condvars, and
the main thread waits 250 ms for every registered thread (`snap_thread_enter`
/ `snap_thread_exit`) to park. If one doesn't, the world resumes and the
request is retried at the next safepoint, up to 8 times. The last attempt
logs the stuck thread, which then logs its own backtrace (SIGUSR2). Twice,
with no reproduction since, the winmm timer thread (role 2) failed to park
within 2 s while inside its callback `f_004b95b0` → `f_004b85a0`, which
makes game C++ virtual calls (input manager). If it happens again, read that
backtrace.

**Chain check.** Every parked thread records its host call chain with
`backtrace` + `dladdr`, keeping only frames inside our executable:

- `f_XXXXXXXX+off` for recompiled functions;
- the bare name for runtime functions.

On load, chains must match per thread (role, key), otherwise the load is
refused before anything changes. Timer threads are compared by TEB
(`fs_base`) instead of by chain.

**Snapshot format.** `"LR2STATE"`, version, build id (hash of the function
table), then `{tag, len, data}` sections. Load runs as follows:

1. Run every section's `check()`.
2. Run `drop()` in reverse order.
3. Run `load()` in order.

All `SnapLock`s are held throughout, for example the audio mixer lock.

**Slots.** F5 saves, F9 loads, F6/F7 choose the slot (0–9). On a gamepad,
hold Back and press LB to save or RB to load. Snapshots stay in memory, raw,
and a writer thread compresses them to `<save>/states/slotN.lr2s`: magic
`LR2Z`, a u64 raw length, then a zlib level 1 stream. zlib is built from
`third_party/zlib` as `lr2_zlib` in `CMakeLists.txt`. A per-slot mutex is
held for the whole write, and `atexit` waits on all of them. Keys are
swallowed by a user32 event hook.

**Measured, in a race:** 87 MiB raw, saved in about 120 ms on the main
thread plus about 800 ms of background compression to 27 MiB. Loading takes
about 30–45 ms from memory. Tested and working:

- saving and loading in the same race;
- loading the same slot twice;
- loading a menu state during a race, and a race state from the menu;
- loading a race state from disk in a freshly started process;
- the F5/F7/F9 key path.

**Guest clock.** `guest_time_ns()` in `kernel32.c` adds an offset to the
monotonic clock. It feeds `GetTickCount`, `timeGetTime`, QPC, `rdtsc` and the
DInput timestamps. A load restores the saved time, so the game sees no jump.

### Sections

| File | Section | Contents |
|---|---|---|
| `savestate.c` | `MEMR` | Guest memory page ranges, zero pages elided: PEB/env, main stack, image, used thread-stack slots, committed VA pages, and the heap up to `heap_top()`. |
| `savestate.c` | `THRD` | Per-thread CPU (up to `CPU.thread`), TLS slots, tid, last error, chain. |
| `memory.c` | `VMEM` | VA tables. |
| `memory.c` | `THNK` | Thunk table by name. Rebuilt by resolving names through the current table, then `com_vtbl_method("IFoo[i]")`, then `module_export_fn`. |
| `win32/com.c` | `COM_` | Guest vtable addresses, then every object as `id, guest, refs, type, payload` through a per-type `ComSnapClass {type, save, load, link, drop}`. Cross-object pointers are stored as guest addresses and fixed in `link()` with `COM_LINK`. `drop()` must only free host resources; it must not `gfree` or release, because guest memory is being replaced. |
| `win32/sync.c` | `HNDL` | Handle table (files reopened by path + offset + `F_GETFL` flags), TLS-slot and stack-slot allocation, next tid. |
| `win32/kernel32.c` | `K32_` | Guest clock, cmdline/env pointers, cwd (`path_get_cwd` / `path_set_cwd`). |
| `win32/winmm.c` | `TIMR` | Timer settings. Timer threads must already match by id. |
| `d3d8/device.c` | `D3DS` | State blocks, the recording block, palettes. |
| `d3d8/device.c` | `ComSnapClass "IDirect3DDevice8"` | Recreates GL objects through `device_gl_setup()`, re-applies window size if `pp` differs, sets `g_dev` and `dirty = ~0`. |
| `d3d8/resources.c` | classes for Surface, Texture, CubeTexture, VB, IB | Descriptions only; contents are in guest memory. GL objects are recreated lazily (`dirty`). Vtables are looked up by name at each creation (`vt_tex()` etc.) because a load can move them. |
| `d3d8/d3d8.c` | `IDirect3D8` | No payload. |
| `audio/dsound.c` | classes for DSound, SoundBuffer, the 3 sub-interfaces; `DSND` | Buffers: format, play state, cursor, 3D params, notifications. Duplicates share a `SoundData`, found again by guest `mem`. `DSND` holds the listener and master volume; its drop empties `s_bufs` before the COM drop. A `SnapLock` holds the mixer. |
| `input/dinput8.c` | classes for DInput and DiDevice; `DINP` | Object tables are rebuilt from the device kind, then per-object range, deadzone, saturation and `fmt_ofs` are applied. Buffered events and pending mouse motion are dropped on load, because input is live. |
| `win32/advapi32.c` | `REG_` | Open key handles. Values live in the registry file and are not rewound. |

`COM_VTABLE(name, arr)` (in `com.h`) registers vtable definitions at startup
through a constructor; `com_vtbl()` refuses undeclared vtables. **Every
`com_vtbl` call site must have a matching `COM_VTABLE`, and every COM type
needs a `ComSnapClass`** (saving a type without one is `rt_fatal`). New COM
types, such as DirectShow, must add both.

Automated tests: `LR2_AUTOINPUT` accepts `N:save:S` and `N:load:S`. Autoinput
counts Presents in the current process rather than the device frame counter,
which a load rewinds, and each command fires once.

### Savestate follow-ups (optional)

- An on-screen message or thumbnail on save/load. Today it is only logged.
- Holding Back for the pad combo still sends Back to the game, and polled
  LB/RB state still sees the press.
- Render-target contents are lost until the next frame redraws them.
- Find handles are not restored.
- States are tied to the build id of the recompiled exe. Runtime frames are
  compared by name, so most runtime-only changes keep old states loadable.



## Other remaining work

- **Controller support:** run the new layout (virtual pad on Linux, a real
  pad on Windows): driving, every race action, menus (A/B/Start/d-pad),
  two pads, and the savestate combo (Back+LB/RB). Check the game's
  joystick options screen with the new button names. Menu actions 2-5
  (keyboard numpad 2/8/4/6) are mapped to the right stick but where the
  game uses them is not known. Pads connected after startup are not seen
  by the game.
- **Movies:** DirectShow `CLSID_FilterGraph` through `com_register_class`,
  possibly decoding with pl_mpeg. DirectMusic if it is ever called.
- **D3D:** P8 textures and `convert_level` in `resources.c`, which are not
  yet advertised.
- **macOS:**
  - the Homebrew build in `README.md` (`brew install cmake ninja sdl3`)
    works on an Apple Silicon MacBook; Intel Macs not tried;
  - a third_party fetch script for SDL3;
  - CI or a build matrix covering macOS arm64 and Windows x64/ARM64, and
    maybe WASM (the 4 GiB guest space needs memory64 or a smaller arena).
- **Windows:** run the ARM64 build on real hardware; try a native build on
  Windows (MSYS2 CLANG64/CLANGARM64 or llvm-mingw's Windows-hosted
  release with the same CMake options; not tried); code signing to avoid
  the SmartScreen prompt.
