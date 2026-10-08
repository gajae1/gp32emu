# Building GP32emu

These instructions are for a source checkout. Run commands from the repository
root. For packaged binaries, see the [README](../README.md) and
[Korean manual](MANUAL.ko.md).

The sources build as C23 and fall back to C11 on older compilers. Configure with
`-DGP32EMU_REQUIRE_C23=ON` to fail instead of falling back.

## Libretro core

The CMake project supports Linux, Windows and the Android NDK:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF
cmake --build build --parallel
```

This produces `gp32emu_libretro.so` on Linux and `gp32emu_libretro.dll` on Windows.
With a multi-configuration generator, select Release when building with
`cmake --build build --config Release --parallel`.

Android requires the NDK toolchain configuration; CMake produces
`gp32emu_libretro_android.so`, while `jni/Android.mk` produces
`libs/<abi>/libretro.so`. See [ANDROID_BUILD.md](ANDROID_BUILD.md) for the Android
steps. `Makefile.libretro` also builds the legacy Linux core.

The Android build script accepts `arm64-v8a` and `armeabi-v7a`. Building `x86_64`
requires a manual NDK CMake configuration with `-DANDROID_ABI=x86_64` or
`ndk-build`. The Android target is API 21. A limited Android app-sandbox run is
documented in [ANDROID_BUILD.md](ANDROID_BUILD.md#runtime-validation); RetroArch
integration and native ARM device use remain unverified.

For Linux AArch64 cross-compilation, put Zig 0.13.0 on PATH and use the toolchain
file in `cmake/toolchains/`. It targets Cortex-A53 and glibc 2.17 or newer.

`scripts/package_release.ps1` assembles a fresh distribution directory from the
four build directories and SDL3 inputs. Pass `-StripTool` with the path to
`llvm-strip` (included in the Android NDK). It removes debug information only
from packaged `.so` copies; original build outputs keep their symbols. Tests,
build caches and local results are not copied into the distribution.

## Standalone frontends

Enable the desired frontend in the same CMake configuration:

| Option | Target |
| --- | --- |
| `-DGP32EMU_BUILD_HEADLESS=ON` (default) | `gp32_headless`, a scriptable runner |
| `-DGP32EMU_BUILD_SDL3=ON` | `gp32_sdl3` |
| `-DGP32EMU_BUILD_SDL12=ON` | `gp32_sdl12` |
| `-DGP32EMU_BUILD_QT6=ON` | `gp32emu_qt6` on Linux |
| `-DGP32EMU_BUILD_WIN64_FRONTEND=ON` | `gp32emu_win64`, requiring SDL3 |

Plain Makefile alternatives are also available:

```sh
make -f Makefile.sdl sdl3
make -f Makefile.sdl sdl12
make -f Makefile.linux qt
make -f Makefile.linux appimage
make -f Makefile.win64 sdl
make -f Makefile.win64
```

The MinGW-w64 build through `Makefile.win64` builds the bundled SDL3 and links it
statically into `GP32emu-win64.exe`. It uses D3D11/GDI video, WASAPI/waveOut audio
and SDL3 joystick input. The CMake Windows executable is `gp32emu_win64.exe` and
uses a separate `SDL3.dll`.

The SDL3 source archive is `third_party/sdl/SDL3-3.4.10.tar.gz`, containing its
`LICENSE.txt`. Preserve the SDL3 notice when distributing either build.
See the [README licensing section](../README.md#credits-and-licensing) for the
project's upstream licensing status and other notices.

## Browser build

```sh
make -f Makefile.wasm
make -f Makefile.wasm serve
```

This builds `web/gp32_wasm_core.wasm`. Open `http://127.0.0.1:8008/` for the local
page, which accepts BIOS and game files by drag and drop. The build needs a C
compiler targeting `wasm32` plus `wasm-ld`, such as clang or
`zig cc -target wasm32-freestanding`. It uses the interpreter without JIT.
Browser playback and Qt runtime use remain unverified.

## Headless runner and tests

```sh
./gp32_headless --bios gp32166m.bin --smc game.smc --frames 3000 --jit --dump-frame frame.ppm
```

Configure with `-DGP32EMU_BUILD_TESTS=ON`, build, then run:

```sh
ctest --test-dir build --output-on-failure
```

For a multi-configuration build, also pass `-C Release` to CTest. The existing
suite covers CPU and JIT behavior, the SoC, audio delivery, save states, file
handling and libretro entry points. `-DGP32EMU_BUILD_BENCHMARK=ON` builds the
optional `gp32_bench` runner and library probes.

## Source layout

- `src/arm920t.c`, `src/arm920t_jit_*`: CPU, interpreter and JIT.
- `src/s3c2400.c`: SoC, including LCD, timers, DMA, audio, interrupts and NAND control.
- `src/gp32.c`, `src/smc_direct.c`: machine, BIOS boot and direct card loading.
- `src/smartmedia.c`: SmartMedia card handling and card state storage.
- `src/libretro/`: libretro core.
- `src/win64/`: Windows standalone frontend.
- `wasm/`, `web/`: browser bridge and page.
- `tests/`: CPU, JIT, SoC, audio, save-state and frontend tests.
- `docs/`: user and developer documentation; `scripts/`: build and probe helpers.
