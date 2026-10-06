# Android libretro core

The core builds for Android through the standard NDK paths. Both need a C23-capable
Clang, and the CMake route also needs CMake and Ninja on PATH. The tested toolchain is
NDK r28b (28.2.13676358) with `APP_PLATFORM` android-21.

## ndk-build

Run `ndk-build` from the project root; it picks up `jni/Android.mk` and
`jni/Application.mk` and writes the standard `libs/<abi>/libretro.so`. Three ABIs are
declared:

- `arm64-v8a` and `x86_64` compile the native backend for their host CPU.
- `armeabi-v7a` has no host backend and uses the portable interpreter.

`jni/Android.mk` exports only the `retro_*` API (through `src/libretro/libretro.map`,
with `--no-undefined`) and sets the 16 KB page-size flags that Android 15 wants on the
two 64-bit ABIs.

## CMake

From PowerShell, with the NDK installed:

```powershell
./scripts/build_android.ps1 -Ndk /path/to/android-ndk
```

The script builds `arm64-v8a` and `armeabi-v7a` into separate directories under
`build-android/`, produces `gp32emu_libretro_android.so`, and writes their SHA-256
values to `manifest.json`. Use `-Abi`, `-OutputDirectory`, `-Jobs` or `-ApiLevel` to
override the defaults; `ANDROID_NDK_HOME` can supply the NDK directory.

## What the build does not prove

All three ABIs link, but the core has never been run on an Android device or emulator
here: runtime loading, frontend import, audio output and executable-memory policy are
still untested. BIOS and games are not distributed with the core.

