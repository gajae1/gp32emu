# Android builds

These instructions are for developers building from source. For installing and
using released packages, see the [README](../README.md) and
[Korean manual](MANUAL.ko.md#android-앱과-retroarch).

## Standalone APK

The standalone app hosts the same libretro core with Android SurfaceView,
AudioTrack and a multitouch control overlay. It uses Java platform APIs and a
C23 JNI bridge, with no Gradle or third-party Android UI dependency.

With Python 3.10+, a JDK, CMake/Ninja, SDK platform 36, build-tools 36.0.0 and
NDK 28.2.13676358 installed:

```sh
python scripts/build_android_app.py --sdk /path/to/Android/Sdk --keystore /path/to/existing.keystore --output-dir /path/to/artifacts
```

This builds ARM64 and ARMv7 from source and produces
`gp32emu-1.0.1-android.apk` (min API 23, target API 35). The app is not debuggable.
The script verifies ELF/ZIP 16 KB alignment and APK signatures. A missing
keystore is an error; it never creates a signing key. Defaults use an existing
`~/.android/debug.keystore` and Android's development alias/password. Other keys
use `GP32EMU_KEY_ALIAS`, `GP32EMU_KEYSTORE_PASSWORD` and `GP32EMU_KEY_PASSWORD`.
Keep signing keys for subsequent updates; do not commit them.

`--test` also builds a same-key instrumentation APK. Install both on a test
device and run `adb shell am instrument -w org.gp32emu.app.test/org.gp32emu.app.InputTest`.
`--debug` creates a separate private `-debug.apk`, requiring `adb install -t`.
Build intermediates stay in the chosen artifact directory outside the checkout.

## Libretro core

The core builds for Android through the standard NDK paths. Both need a C23-capable
Clang, and the CMake route also needs CMake and Ninja on PATH. The tested toolchain is
NDK r28b (28.2.13676358) with `APP_PLATFORM` android-21.

### ndk-build

Run `ndk-build` from the project root; it picks up `jni/Android.mk` and
`jni/Application.mk` and writes the standard `libs/<abi>/libretro.so`. Three ABIs are
declared:

- `arm64-v8a` and `x86_64` compile the native backend for their host CPU.
- `armeabi-v7a` has no host backend and uses the portable interpreter.

`jni/Android.mk` exports only the `retro_*` API (through `src/libretro/libretro.map`,
with `--no-undefined`) and sets the 16 KB page-size flags that Android 15 wants on the
two 64-bit ABIs.

### CMake

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

## Validation limits

The standalone app requires API 23; the separate libretro cores target API 21.
Do not package the app's API 23 native libraries as API 21 core downloads.
ARM64 has a native JIT; ARMv7 uses the interpreter.

Android 11 AVD checks cover input mapping, persistence, rotation and pause.
Emulator checks do not establish native ARM performance or audible quality.
Physical Android devices, ARMv7 runtime, Android RetroArch integration and
16 KB-page devices still need runtime validation. BIOS and games are not
included in the app, core or instrumentation package.
