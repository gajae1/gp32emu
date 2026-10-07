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
override the defaults; `ANDROID_NDK_HOME` can supply the NDK directory. The script
covers only `arm64-v8a` and `armeabi-v7a`; its `-Abi` parameter accepts those two
and the release packages them. For `x86_64`, configure CMake manually with
`-DANDROID_ABI=x86_64` or use the `ndk-build` route above, which builds all three.

## Runtime validation

The released 1.0.0 ARM64 core has been loaded by a private JNI/libretro test frontend
inside an Android 11 app sandbox. This used an x86-64 Android Virtual Device with
`libndk_translation.so` and 4 KB pages, not a native ARM phone.

An Astonishia Story R title save state ran for 180 frames. All 132,299 stereo audio
frames (44.1 kHz, signed 16-bit) matched the Windows reference byte for byte. The
core also serialized and restored a fresh state successfully. JIT was requested,
but the test did not independently measure whether native JIT code executed.

This checks library loading, the exercised core path, audio callbacks and state
compatibility. It does not verify RetroArch import, on-screen presentation, audio
device playback, controller input, native ARM performance, ARMv7 runtime, or a
16 KB-page device. Those still need testing. BIOS and games are not distributed
with the core or the test frontend.

