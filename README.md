# GP32emu

A Game Park GP32 emulator written in C. The GP32 is the 2001 Korean handheld built around a
Samsung S3C2400 SoC (ARM920T core). One emulation core serves a libretro core, a headless runner
and several standalone frontends.

This repository is a fork of the original gp32emu project by gameblabla
(https://github.com/gameblabla/gp32emu) and is maintained as
https://github.com/gajae1/gp32emu. The fork is about speed: the original targets desktop machines,
and this one also tries to reach full speed on low-end ARM hardware such as cheap handhelds and
phones. The credits and licence terms are at the bottom of this file.

No BIOS, game ROMs or other copyrighted files are included. Bring your own dumps.

## Release 1.0.0

1.0.0 is the first release and replaces the 1.0.0-rc1 draft build. The archive is published on the
project's [Releases page](https://github.com/gajae1/gp32emu/releases) and groups the binaries by
target:

- `windows-x64/`: the standalone `gp32emu_win64.exe` (keep `SDL3.dll` beside it), the libretro DLL
  and the core `.info`.
- `linux-aarch64/`: the AArch64 libretro core and `.info` for RetroArch handhelds.
- `android-arm64/`, `android-armv7/`: Android cores and `.info`. ARMv7 runs the interpreter, so it
  is much slower than the ARM64 core.

`manifest.json` lists a SHA-256 hash for every file, and the archive carries the third-party notices
and this documentation. ROMs, BIOS files and saves are not included. RetroArch installation steps
are under [Using the core](#using-the-core); the Korean manual is
[docs/MANUAL.ko.md](docs/MANUAL.ko.md).

The release includes wide resampler accumulation, filter-table reuse after an underrun,
and mid-frame or batched PCM delivery.
Tests cover them, but listening on the handheld is still unverified.

## What it does

- ARM920T CPU: a portable interpreter plus a dynamic recompiler (JIT) for x86-64 and little-endian
  AArch64. 32-bit ARM hosts and the WebAssembly build run the interpreter only.
- Boots a real GP32 BIOS (v1.6.6, `gp32166m.bin`), or loads SmartMedia cards directly without one.
- Content: `.smc` cards, `.fxe` and `.fpk` homebrew.
- Frontends: libretro core (main target), SDL 1.2, SDL3, Qt6 on Linux, Win64, and a WebAssembly
  build for browsers.
- Headless runner and benchmark tools, used for the tests and for the measurements below.
- Save states, SmartMedia write-back, optional LCD persistence (FLU ghosting) and frame
  interpolation.
- Optional guest CPU speed from 100% to 300%, which gives a game more CPU time without changing
  sound pitch, timers or screen refresh.

## Speed

Speed was tuned against a 1.5 GHz Cortex-A53 handheld, the slowest hardware this project aims at.
The numbers below were recorded during development on that machine with the headless bench
harness: BIOS boot, JIT on, core only, so RetroArch's own video and audio drivers are not part of
the measurement. These benchmarks use 60 host steps per second as the real-time baseline;
the emulated LCD refresh follows the game's clock and LCD register settings.

| Scene | Core frames per second |
| --- | --- |
| Astonishia Story R, opening movie | about 96 |
| Little Girl Mill, card loading | about 79 |
| Blue Angelo, town dialogue | about 85 |
| Princess Maker 2, cutscene | about 120 |

These are short bench scenes, not full-game averages. Little Girl Mill spends most of its window
streaming card data, and Astonishia's opening movie is the heaviest sustained workload that has
been measured. The scenes, and the runs behind them, are recorded in
[docs/DEVELOPMENT_STATUS.md](docs/DEVELOPMENT_STATUS.md).

On the compatibility side, 28 commercial cards were run for 3,600 frames each in both the JIT and
the interpreter with scripted input: nothing crashed, and the two engines agreed on every card.
That is a check that games start and run, and that both engines emulate the same machine. It is
not proof that every game is playable from start to finish.

## What is verified, and what is not

| Target | State |
| --- | --- |
| Windows x64 | Built and smoke-tested: the Win64 frontend and the libretro DLL build with the Zig C23 toolchain, and the regression suite passes there. |
| Linux x86-64 | Built and tested: libretro core, headless runner and the regression suite. |
| AArch64 Linux (Cortex-A53 class, glibc 2.17 or newer) | Cross-compiled with the Zig toolchain in `cmake/toolchains/`. The 1.0.0 JIT, audio and persistence regressions pass on native Cortex-A53 hardware, along with three bounded game-scene runs. CPU regression and game-output comparisons also run under QEMU; its timings are not device performance estimates. |
| Android arm64-v8a, armeabi-v7a, x86_64 | Cross-compiled with NDK 28.2 (`APP_PLATFORM android-21`) through both the CMake toolchain and `jni/Android.mk`; all three link. The release packages the two ABIs `scripts/build_android.ps1` accepts, arm64-v8a and armeabi-v7a; x86_64 needs a manual CMake configure (`-DANDROID_ABI=x86_64`) or `ndk-build`. Never run on a device or an emulator. |
| 32-bit ARM | Builds and runs the interpreter only. There is no 32-bit ARM recompiler, so it is too slow for most games. |
| WebAssembly | Interpreter only. The core and the JS bridge build, but browser playback has not been verified. |

## Known limitations

- Booting without a BIOS works for most cards but not all of them. Astonishia Story R stalls in
  its start-up code, and Hany and Super Plusha do not match the BIOS read because their payloads
  are packed in a way the direct loader does not decrunch. Use the BIOS when you have one; it is
  the reference path.
- Pinball Dreams starts and plays without a BIOS, but its sound is silent: the game's own codec
  setup leaves the volume at 0x3f and nothing raises it again.
- Save states carry RAM and the whole SmartMedia image, which is why a slot is around 10 MB.
  Libretro serialization is memory-only, so full-speed rewind or run-ahead is not established,
  and rewind/run-ahead inside a real RetroArch session has not been verified.
- No measurement against a physical GP32 has been recorded. Sound and timing follow the S3C2400
  manual and the observed BIOS behaviour. The source of remaining clicks and pops has not been
  established for every scene; speaker-level fidelity, input latency and long playthroughs are
  still open.
- The compatibility work covers bounded scenes (3,600 frames of scripted input per card), not
  complete games.

## Building

The sources build as C23 and fall back to C11 on older compilers. Configure with
`-DGP32EMU_REQUIRE_C23=ON` to fail instead of falling back.

### Libretro core

Works with CMake on Linux, Windows and the Android NDK:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF
cmake --build build --parallel
```

That produces `gp32emu_libretro.so` on Linux and `gp32emu_libretro.dll` on Windows. Android via CMake
produces `gp32emu_libretro_android.so`; the `jni/Android.mk` route produces the standard
`libs/<abi>/libretro.so`. Android steps are in [docs/ANDROID_BUILD.md](docs/ANDROID_BUILD.md), and
the core's own notes are in [docs/libretro/README_LIBRETRO.md](docs/libretro/README_LIBRETRO.md).
`Makefile.libretro` still builds the legacy Linux core.

An AArch64 Linux handheld can be cross-compiled from another machine with Zig 0.13.0 on PATH and
the toolchain file in `cmake/toolchains/`; it targets Cortex-A53 / aarch64 Linux with glibc 2.17 or
newer.

### Standalone builds

The same CMake project builds the standalone frontends; each one is an option:

- `-DGP32EMU_BUILD_HEADLESS=ON` (default) builds `gp32_headless`, the scriptable runner.
- `-DGP32EMU_BUILD_SDL3=ON` builds `gp32_sdl3`; `-DGP32EMU_BUILD_SDL12=ON` builds `gp32_sdl12`.
- `-DGP32EMU_BUILD_QT6=ON` builds the Qt6 GUI on Linux (`gp32emu_qt6`).
- `-DGP32EMU_BUILD_WIN64_FRONTEND=ON` builds the Win64 GUI (`gp32emu_win64`, SDL3 required).

There are plain Makefiles as well:

```sh
make -f Makefile.sdl sdl3          # SDL3 window
make -f Makefile.sdl sdl12         # SDL 1.2 window
make -f Makefile.linux qt          # Qt6, or: make -f Makefile.linux appimage
make -f Makefile.win64             # GP32emu-win64.exe, MinGW-w64 cross build
make -f Makefile.wasm              # browser build, then: make -f Makefile.wasm serve
```

`Makefile.win64` builds the bundled SDL3 first (`make -f Makefile.win64 sdl`) and links D3D11/GDI
video, WASAPI/waveOut audio and SDL3 joystick input. It names the binary `GP32emu-win64.exe`; the
Win64 GUI shipped in the release archives is the CMake build above, which produces
`gp32emu_win64.exe` and loads a separate `SDL3.dll`. The WebAssembly build needs any C compiler
that can target `wasm32` plus `wasm-ld` (clang, or `zig cc -target wasm32-freestanding`).

### Headless runner and tests

```sh
./gp32_headless --bios gp32166m.bin --smc game.smc --frames 3000 --jit --dump-frame frame.ppm
./gp32_bench --bios gp32166m.bin --smc game.smc --warmup 2400 --frames 300 --jit
```

Configure with `-DGP32EMU_BUILD_TESTS=ON` and run `ctest --test-dir build --output-on-failure`. The
suite covers the CPU, the JIT, the SoC (timers, PWM, LCD, GPIO, IIS/DMA, interrupts, EEPROM),
audio delivery, save states, the file layer and the libretro entry points.
`-DGP32EMU_BUILD_BENCHMARK=ON` adds `gp32_bench` and the library probes.

## Using the core

Korean manual: [docs/MANUAL.ko.md](docs/MANUAL.ko.md). The short version for RetroArch:

1. Put `gp32emu_libretro.so` (or `.dll`) in RetroArch's cores directory.
2. Put your BIOS in the system directory under the name `gp32166m.bin`.
3. Load a `.smc`, `.fxe` or `.fpk` file with the GP32emu core.

Windows standalone: run `gp32emu_win64.exe` with `SDL3.dll` beside it. `File > Open BIOS`,
`Open SmartMedia image`, `Open FXE` and `Open FPK` load content, `Config > Set BIOS path` remembers
a BIOS, F5/F8 save and load state, and F12 takes a screenshot. The same options are on the command
line: `gp32emu_win64.exe --bios gp32166m.bin --smc game.smc` (also `--fxe`, `--fpk`, `--jit`,
`--no-jit`).

When booting with a BIOS, Windows standalone preserves in-game SmartMedia saves beside the original card as
`game.smc.gp32.smc` and restores that file on the next load. The original card stays unchanged;
its directory must be writable. Save failures are reported. F5/F8 states are separate.
BIOS-less direct boot has no writable card device; use save states for that path.

The core options are the dynamic recompiler, the boot mode (`auto`, `require_bios` or `direct_hle`),
LCD persistence, frame interpolation and the CPU speed; the manual explains each one, and
[docs/CPU_SPEED_OPTION.md](docs/CPU_SPEED_OPTION.md) has the measurements behind the speed option.
SmartMedia saves are written next to RetroArch's normal save files as `<game>.gp32.smc` when the
content is closed.

## Source layout

- `src/arm920t.c`, `src/arm920t_jit_*`: CPU, interpreter, JIT
- `src/s3c2400.c`: the SoC (LCD, timers, DMA, IIS audio, interrupts, NAND controller)
- `src/gp32.c`, `src/smc_direct.c`: the machine, BIOS boot, direct card loading
- `src/libretro/`: the libretro core
- `wasm/`, `web/`: the browser build and its page
- `tests/`: unit tests for the CPU, JIT, SoC, audio and save states
- `docs/`: development notes; `scripts/`: build and probe helpers

## Credits and licensing

GP32emu was started by gameblabla, and the original project lives at
https://github.com/gameblabla/gp32emu. This fork keeps those commits in its history. The upstream
repository does not state a licence for its own code at the time of writing, so this repository
does not add one either; ask the upstream author before reusing it outside GitHub's fork terms.

Parts of the hardware model come from MAME (BSD-3-Clause, Tim Schuerewegen, Raphael Nabet and
others). The repository also includes the MIT-licensed kuba-- zip library and parts of BDMEmu's
frontend. Their notices are in [licenses/](licenses/). The Windows archive ships SDL3 under the
zlib licence; its notice is in [third_party/sdl/SDL3-3.4.10.tar.gz](third_party/sdl/SDL3-3.4.10.tar.gz)
as `LICENSE.txt` and travels beside the DLL as `SDL3-LICENSE.txt`. SDL3 is fetched as a release
asset rather than installed separately, and the static MinGW build links it into the executable.

Game ROMs and BIOS files are not included and are not distributed with the emulator; supply your
own dumps.

