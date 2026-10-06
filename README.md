# GP32emu

An emulator for the Game Park GP32 (2001, ARM920T handheld), written in C. It is a fork of
[gameblabla/gp32emu](https://github.com/gameblabla/gp32emu), aimed at running GP32 games smoothly on
cheap ARM handhelds (Allwinner H700, Rockchip RK3326 and similar) through RetroArch.

No BIOS, games or other copyrighted files are included. Bring your own dumps.

## What it does

- ARM920T interpreter plus a recompiler (JIT) for x86-64 and AArch64.
  32-bit ARM hosts use the interpreter only.
- Boots the real GP32 BIOS (v1.6.6, `gp32166m.bin`), or loads SmartMedia cards directly without a BIOS.
- Frontends: libretro core (main target), SDL 1.2, SDL3, Qt6/Linux, Win64, and a WebAssembly build for browsers.
- Save states, SmartMedia save writeback, optional LCD persistence and frame interpolation.
- An optional CPU speed setting (100% to 300%) that gives the game more CPU time without changing
  sound pitch or timers. It helps games that are slow on the original console.

## Where it stands

Measured on an RG35XX SP class device (H700, Cortex-A53 at 1.5 GHz), real BIOS boot, JIT on,
core only (RetroArch's own video and audio drivers are not included in these numbers):

| Scene | Frames per second |
| --- | --- |
| Astonishia Story R, opening | about 96 |
| Little Girl Mill, loading from card | about 79 |
| Blue Angelo, town dialogue | about 85 |
| Princess Maker 2, cutscene | about 120 |

All 28 commercial cards that were available for testing start and run for 3600 frames of scripted
input with no crash, and the JIT and the interpreter produce identical output on each of them. Most
games run at 110 to 240 fps on the same device. That is a check that games run and agree with the
interpreter, not proof that every game is playable from start to finish.

Known gaps:

- Booting without a BIOS works for many cards but not all. A few stay in their start-up code
  (Astonishia Story R, Holeman Battle Race 2002, Tales of Windy Land, Topy Topy Gogo among them).
  Use the BIOS when you have it; it is the reference path.
- Pinball Dreams starts and plays without a BIOS but its sound is silent.
- Android builds link but have not been run on a device.
- Nothing has been compared against a physical GP32 side by side. Sound and timing follow the
  S3C2400 manual and BIOS behaviour, and some noise in sound comes from the games' own data.
- No 32-bit ARM recompiler. Boards slower than a Cortex-A53 fall back to the interpreter, which is
  too slow for most games.

More detail is in [docs/DEVELOPMENT_STATUS.md](docs/DEVELOPMENT_STATUS.md).

## Using it

Korean manual: [docs/MANUAL.ko.md](docs/MANUAL.ko.md). Short version for RetroArch:

1. Put `gp32emu_libretro.so` (or `.dll`) in RetroArch's cores folder.
2. Copy your BIOS to the system folder under the name `gp32166m.bin`.
3. Load a `.smc`, `.fxe` or `.fpk` file with the GP32emu core.

For SpruceOS on H700 devices see [packaging/spruce/README.md](packaging/spruce/README.md).

## Building

The code builds as C23 and falls back to C11 on older compilers.

Libretro core with CMake (Linux, Windows, Android NDK):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF
cmake --build build --parallel
```

H700 and other AArch64 Linux handhelds can be cross compiled with Zig 0.13.0 using
`cmake/toolchains/h700-zig.cmake`. Android steps are in [docs/ANDROID_BUILD.md](docs/ANDROID_BUILD.md).
Full libretro notes are in [docs/libretro/README_LIBRETRO.md](docs/libretro/README_LIBRETRO.md).

Other frontends:

```sh
make -f Makefile.sdl sdl3          # SDL3 window
make -f Makefile.sdl sdl12         # SDL 1.2 window
make -f Makefile.linux qt          # Qt6
make -f Makefile.win64             # Windows, cross build
make -f Makefile.wasm              # browser build, then: make -f Makefile.wasm serve
```

The WebAssembly build uses the interpreter only and takes any C compiler that can target
`wasm32` plus `wasm-ld` (clang, or `zig cc -target wasm32-freestanding`).

Headless runner, useful for tests and benchmarks:

```sh
./gp32_headless --bios gp32166m.bin --smc game.smc --frames 3000 --jit --dump-frame frame.ppm
./gp32_bench --bios gp32166m.bin --smc game.smc --warmup 2400 --frames 300 --jit
```

Run the tests with `ctest --test-dir build --output-on-failure` after configuring with
`-DGP32EMU_BUILD_TESTS=ON`.

## Source layout

- `src/arm920t.c`, `src/arm920t_jit_*`: CPU, interpreter, JIT
- `src/s3c2400.c`: the SoC (LCD, timers, DMA, IIS audio, interrupts, NAND controller)
- `src/gp32.c`, `src/smc_direct.c`: the machine, BIOS boot, direct card loading
- `src/libretro/`: the libretro core. `wasm/`, `web/`: browser build
- `tests/`: unit tests for the CPU, JIT, SoC, audio and save states

## Credits and licenses

GP32emu was started by gameblabla. This fork builds on that code, and the original commits are kept
in the history. The upstream repository does not state a license for its own code at the time of
writing, so this repository does not add one either; ask the upstream author before reusing it
outside of GitHub's fork terms.

Parts of the hardware model come from MAME (BSD-3-Clause, Tim Schuerewegen, Raphael Nabet and
others), and the repository includes the MIT-licensed kuba-- zip library and parts of BDMEmu's
frontend. Their notices are in [licenses/](licenses/).

