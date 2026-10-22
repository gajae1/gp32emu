# Building GP32emu

These instructions are for a source checkout. Run commands from the repository
root. For packaged binaries, see the [README](../README.md) and
[Korean manual](MANUAL.ko.md).

The sources build as C23 and fall back to C11 on older compilers. Configure with
`-DGP32EMU_REQUIRE_C23=ON` to fail instead of falling back.

Optional background workers are enabled by default. Thread-enabled POSIX builds
prepare a small reserve of decoded-code storage in the background; guest CPU and
device execution remain ordered on the emulation thread. Other targets allocate
the same storage synchronously. Use `-DGP32EMU_ENABLE_THREADS=OFF` when building
without background workers.

For the iPhone/iPad app, Xcode signing, unsigned IPA packaging and debugger-assisted
JIT, see [Building the iOS app](IOS_BUILD.md).

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
`ndk-build`. The separate Android cores target API 21; the standalone app targets API 23
as its minimum. See [ANDROID_BUILD.md](ANDROID_BUILD.md) for app packaging.

For Linux AArch64 cross-compilation, put Zig 0.13.0 on PATH and use the toolchain
file `cmake/toolchains/linux-aarch64-zig.cmake`. It retains the Cortex-A53
code-generation settings and targets glibc 2.17 or newer. The old
`h700-zig.cmake` name is a compatible alias. The target requires compatible CPU
features, glibc, and a 64-bit libretro frontend. A 32-bit RetroArch process
cannot load this core, even on a 64-bit kernel. Android and Switch need their
own platform builds.

### Profile-guided release builds (Clang)

PGO uses recorded execution counts to optimize the shared C core and libretro
boundary. Linux ARM64 releases use it by default; there is no emulator setting
or runtime collection. Other targets need their own training and validation.
It does not change guest clocks or game code. Source
builds without a training profile build normally. Clang 18 or newer with the
GNU-style driver is required for this CMake workflow.
Keep the compiler version, target CPU, build options and source revision the same
between training and use. Use separate build directories:

```sh
cmake -S . -B build-pgo-train -DCMAKE_BUILD_TYPE=Release \
  -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF \
  -DGP32EMU_PGO_MODE=GENERATE
cmake --build build-pgo-train --parallel
# Run this core in the target frontend with a writable profile destination:
# LLVM_PROFILE_FILE=/absolute/path/profiles/run-%m-%p.profraw <frontend command>
llvm-profdata merge -o games.profdata profiles/*.profraw
cmake -S . -B build-pgo-use -DCMAKE_BUILD_TYPE=Release \
  -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=OFF \
  -DGP32EMU_PGO_MODE=USE -DGP32EMU_PGO_FILE=/absolute/path/games.profdata
cmake --build build-pgo-use --parallel
```

Pass the same toolchain options to both configurations when cross-compiling.
Use `llvm-profdata` from the compiler's LLVM release: raw profile versions are
not interchangeable. A compiler distribution without the target profiling
runtime, such as Zig 0.13 for Linux AArch64, also needs
`-DGP32EMU_PGO_RUNTIME=/absolute/path/target-profile-runtime.a` in the training
build. This is compiler-rt's profile archive built for the target, not a host
library. The use build has no profiling-runtime dependency.

Train on several games, including startup and gameplay, then compare an ordinary
Release build against the use build with identical input and saved states.
Include games outside the training set. Training instrumentation is expensive;
never measure its speed as the optimized result or distribute the training core.
The ELF training core stays mapped until process exit so that a frontend's
`dlclose` cannot invalidate the profile runtime's exit handler. Exit the process
normally to write the profile; forced termination can lose the current counts.

Static function profile names retain repository-relative paths, so the training
and use checkouts may live in different directories. Regenerate profiles after
source or toolchain changes. Incompatible function profiles fail the build, but
this is not a substitute for recording the training revision and options: some
changes can remain structurally compatible while invalidating the old workload.
Each configured PGO build records its mode, compiler and profile SHA-256 in
`gp32-pgo-Release.txt`. Replacing the profile rebuilds affected core objects.
Tests and third-party ZIP code do not inherit PGO compile flags. Frontends other
than libretro still benefit from the shared core, but their UI sources are not
profiled by this option. PGO does not implicitly enable IPO or change CPU flags.
For Linux ARM64 releases, also configure the final build with
`-DGP32EMU_ENABLE_IPO=ON`. Clang uses ThinLTO to optimize across translation units;
the Zig toolchain invokes its own archive tools without external wrapper scripts.
Validate this final configuration against the ordinary PGO build. Other targets
retain the default IPO setting until separately measured.

`scripts/package_release.ps1` assembles five independent ZIP releases from a
clean, committed source tree. Pass a new output directory plus four build
directories: one Windows build containing `gp32emu_win64.exe` and
`gp32emu_libretro.dll`, one Linux AArch64 core build, and one build for each
Android ABI. Also pass SDL3's DLL and licence notice and `llvm-strip` (included
in the Android NDK). Debug information is removed only from packaged `.so`
copies; original build outputs keep their symbols. Binary packages exclude
tests, build caches and local results; source packages retain the tests.
The Linux ARM64 input must be a validated PGO `USE` build with IPO enabled and its generated
`gp32-pgo-Release.txt`; packaging rejects an ordinary or training build. Its
manifest records IPO and the training profile's SHA-256. All other CMake inputs also
reject `GENERATE` mode, so profiling instrumentation cannot be shipped by mistake.

The defaults produce `gp32emu-1.1.0-windows-x64.zip`,
`gp32emu-1.1.0-libretro-windows-x64.zip`, `gp32emu-1.1.0-linux-aarch64.zip`,
`gp32emu-1.1.0-android-arm64.zip` and `gp32emu-1.1.0-android-armv7.zip`.
Each archive places its binaries and, for cores, `gp32emu_libretro.info` at the
root, then adds user documentation, `licenses/` and `manifest.json`.
The output directory also receives `SHA256SUMS.txt` for the generated packages.

## Nintendo Switch (experimental)

`Makefile.libretro` includes a `platform=libnx` target for devkitPro's devkitA64
and libnx. In a configured devkitPro shell, run:

```sh
make -f Makefile.libretro platform=libnx
```

It produces `gp32emu_libretro_libnx.a`, a static core intended for linking with
RetroArch's libnx frontend. It is not a directly loadable `.so` or a standalone
`.nro`. Use a clean build directory or checkout when changing platform targets;
the Makefile shares object paths between platforms.

The [official RetroArch build guide](https://docs.libretro.com/development/retroarch/compilation/switch-libnx/)
describes copying this library into RetroArch as `libretro_libnx.a` and linking
it into an `.nro`. RetroArch does not compile or execute C source on the device.

The Switch port uses libnx JIT memory with separate writable and executable
aliases. The ARM64 recompiler is shared with other AArch64 hosts. If the
homebrew loader does not provide JIT permissions, the core uses the interpreter.
The release includes an experimental NRO linked with RetroArch 1.22.2. Device
testing remains outstanding. Linux AArch64 packages cannot be used on Switch.

The manually dispatched `Switch libnx build` GitHub Actions workflow builds
the static core and NRO using the official `devkitpro/devkita64` container.
It records installed toolchain packages and both source revisions alongside
the artifacts. For a noninteractive shell, add `$DEVKITPRO/devkitA64/bin` to
`PATH` before running make.

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

## Release packaging

Commit the reviewed source before running `scripts/package_release.ps1`. Supply
the four build directories, SDL3 DLL and license, LLVM strip tool, and a new
output directory using the script's named parameters. Build those inputs from
the same source revision; the packaging step does not compile them.

The script produces separate Windows-app and Windows/Linux/Android core ZIPs,
a committed-source ZIP, and `SHA256SUMS.txt`. Optional `-AndroidApk` and `-IosIpa`
arguments add the standalone mobile downloads without nesting them inside ZIPs.
The IPA must be the unsigned public build; provisioning profiles and embedded
executable signatures are rejected. See [iOS builds](IOS_BUILD.md) for preparing it.
Switch NRO and companion source packages use the separate Switch workflow above.

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
