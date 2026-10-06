# GP32emu libretro core

This is a separate libretro backend for GP32emu. It builds a normal libretro shared object named `gp32emu_libretro.so` on Linux and does not replace the SDL, Qt, Win64, or WASM frontends.

## Portable CMake builds

The CMake libretro target uses the same CPU, SoC, audio and media sources as the
headless runner. No SDL or Qt installation is needed.

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF
cmake --build build-core --parallel
```

Linux produces `gp32emu_libretro.so`; Windows produces `gp32emu_libretro.dll`.
To cross-compile for an AArch64 Linux handheld, install Zig 0.13.0 on PATH and
use the aarch64 toolchain file in `cmake/toolchains/`:

```sh
cmake -S . -B build-aarch64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/<aarch64-toolchain>.cmake -DCMAKE_BUILD_TYPE=Release -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF
cmake --build build-aarch64 --parallel
```

This targets Cortex-A53 / aarch64 Linux with glibc >= 2.17, and it is a cross
build only: nothing in this tree has been run on that hardware. It does not
target Android; use the NDK CMake toolchain for that instead (NDK 28.2 tested):

```sh
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 -DCMAKE_BUILD_TYPE=Release -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF
cmake --build build-android --parallel
```

Use `armeabi-v7a` for 32-bit Android. Android output is
`gp32emu_libretro_android.so`; install it using the frontend's supported local
core installation flow. Android builds have been linked, but not device-tested.
The native dynamic recompiler supports x86-64 and little-endian AArch64.
AArch64 combines native ARM instruction blocks with classified helpers for
complex operations and portable acceleration of stable polling loops.
32-bit ARM hosts use the portable translated interpreter. The JIT option
does not enable a native 32-bit ARM recompiler.

## Installing on a Linux handheld

Handheld builds install like any other libretro core: put the AArch64
`gp32emu_libretro.so` where the frontend looks for cores, and put the core-info
metadata into the frontend's own metadata directory, because an info file beside
the core is not always picked up. Games and `gp32166m.bin` go wherever that
frontend keeps its content and BIOS files, and the game list may need a refresh.
The example packaging in this repository is written for one handheld firmware;
other firmware versions need different paths and may need a different launcher.
BIOS and games are not included.

## Regression checks and measurements

Set `GP32EMU_BUILD_TESTS=ON` to build focused audio-delivery, persistent NAND
and polling-equivalence tests; run `ctest --test-dir <build> --output-on-failure`.
For cross builds run the four test executables on the target device.

Set `GP32EMU_BUILD_BENCHMARK=ON` for `gp32_bench`:

```sh
./gp32_bench --bios gp32166m.bin --smc game.smc --warmup 2400 --frames 300
```

For a repeatable scene captured after entering a game, pass a native GP32emu
state file with `--state path/to/state` and use a short warmup. A compressed
RetroArch `.state` or `.state.auto` file is not accepted directly; it contains
a frontend wrapper around the core state. With `--state`, no BIOS auto-start
button pulses are sent. The original state is read without modification.

The default input pulses attempt BIOS menu confirmation. They are not a
universal gameplay script. Use `--input-script path` for a known scene and
compare output pixels/PCM hashes, CPU state and cycle totals before comparing
speed. The timing excludes warmup and includes framebuffer/PCM hashing, but
not RetroArch video/audio drivers. It cannot establish real frontend FPS.

## Legacy Makefile build on Linux

```sh
make -f Makefile.libretro clean all
```

The output is:

```text
gp32emu_libretro.so
```

Install it into your RetroArch cores directory, for example:

```sh
mkdir -p ~/.config/retroarch/cores
cp gp32emu_libretro.so ~/.config/retroarch/cores/
cp gp32emu_libretro.info ~/.config/retroarch/cores/
```

## BIOS placement on Linux

Put the GP32 BIOS in the RetroArch system directory with this exact filename:

```text
~/.config/retroarch/system/gp32166m.bin
```

Example:

```sh
mkdir -p ~/.config/retroarch/system
cp "[BIOS] GamePark GP32 (Europe) (v1.6.6).bin" ~/.config/retroarch/system/gp32166m.bin
```

The core option `Boot mode` defaults to `auto`: it uses `gp32166m.bin` when available and falls back to BIOSless direct SmartMedia boot for `.smc` content if the BIOS is missing. For maximum compatibility, put the BIOS in the system directory and leave Boot mode on `auto`, or set it to `require_bios` if you want missing BIOS to be treated as an error.

## Running a game

With RetroArch installed and the core copied into RetroArch's core directory:

```sh
retroarch -L ~/.config/retroarch/cores/gp32emu_libretro.so "Little Wizard (Europe).smc"
```

When running from the current build directory, include `./` so RetroArch/dlopen treats the core as a path rather than a core name:

```sh
retroarch -L ./gp32emu_libretro.so "Little Wizard (Europe).smc"
```

`--libretro=./gp32emu_libretro.so` also works, but `-L ./gp32emu_libretro.so` is the most common RetroArch command-line form.

From RetroArch's UI: Load Core -> GP32emu, then Load Content -> select a `.smc`, `.fxe`, or `.fpk` file.

## Core options

The core exposes these options through RetroArch's Core Options menu:

- Dynamic recompiler: enabled by default.
- LCD persistence / GP32 FLU ghosting: disabled by default.
- Frame interpolation: disabled by default.
- CPU speed: 100% by default. Higher values give the game more CPU time per
  second without changing sound pitch, timers or screen refresh, which can
  shorten CPU-bound loading or slowdown. It needs proportionally more host CPU;
  on slow handhelds keep 100% unless a scene needs it. See
  `docs/CPU_SPEED_OPTION.md`.
- Boot mode: `auto` by default. `auto` uses BIOS when found, `require_bios` fails clearly if the BIOS is missing, and `direct_hle` forces BIOSless direct/HLE loading.


## Controls

Port 1 maps to a standard RetroPad:

- D-pad -> GP32 stick directions
- A -> GP32 A
- B -> GP32 B
- L -> GP32 L
- R -> GP32 R
- Start -> GP32 Start
- Select -> GP32 Select

Use RetroArch's normal input remapping for controller-specific remaps.

## Save behavior

Libretro serialization uses caller-owned memory and GP32emu's native state
format, without temporary files. SmartMedia writeback is attempted on unload, deinitialization and content
replacement into the RetroArch save directory as `<game>.gp32.smc`. The next
SMC load restores that image before considering the original ROM. Invalid
saved images abort loading instead of silently replacing the user's save.
ROMs with the same basename share this save path, even across content folders.
The save directory must be writable for SmartMedia writeback. State size
queries do not access storage; short serialization buffers are rejected and
larger buffers are zero-padded. The reported size is held stable per loaded
game; unexpected payload growth fails rather than truncating a state. The
payload still includes RAM and the full SmartMedia image, so memory-only
serialization does not establish full-speed rewind or run-ahead support.
State interoperability across architectures has not been established.

Audio accepted only partially by the frontend is retained across calls. A
sustained refusal is bounded to 250 ms of recent PCM; older queued audio is
dropped rather than allowing unbounded memory use or playback latency.

When the emulated LCD produced no new frame during a run, the core presents a
duplicate frame so the frontend keeps video/audio pacing without restaging
identical pixels. A NULL frame is sent only when the frontend advertises
`GET_CAN_DUPE`; otherwise the last presented pixel buffer is resent. LCD
persistence and frame interpolation still process every
frontend frame because their output varies over time.

## Troubleshooting

If RetroArch immediately returns to the shell, run with logging enabled:

```sh
retroarch --verbose -L ./gp32emu_libretro.so "Little Wizard (Europe).smc"
```

Also make sure the core path includes either an absolute path or `./`. On Linux, `gp32emu_libretro.so` without a slash may be interpreted as a core name or searched through RetroArch's configured core directories rather than the current directory.
