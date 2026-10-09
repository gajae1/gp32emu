# Android builds

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
`gp32emu-1.0.0-android.apk` (min API 23, target API 35). The app is not debuggable.
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

The standalone APK additionally passes framework input checks on that Android
11 AVD: touch visibility, all GP32 buttons, simultaneous touch/keyboard/gamepad
holds, pointer release/cancel, focus loss, rotation and pause. The system file
picker imported a BIOS and Astonishia Story R, the BIOS launcher accepted touch
input, and the game's opening displayed through SurfaceView. AudioTrack was
active at 44.1 kHz stereo. The AVD was muted; this does not establish audible
quality, real-phone latency or sustained native ARM performance.

The released 1.0.0 ARM64 core has been loaded by a private JNI/libretro test frontend
inside an Android 11 app sandbox. This used an x86-64 Android Virtual Device with
`libndk_translation.so` and 4 KB pages, not a native ARM phone.

An Astonishia Story R title save state ran for 180 frames. All 132,299 stereo audio
frames (44.1 kHz, signed 16-bit) matched the Windows reference byte for byte. The
core also serialized and restored a fresh state successfully. JIT was requested,
but the test did not independently measure whether native JIT code executed.

The earlier core-only check covers library loading, the exercised core path,
audio callbacks and state compatibility. Neither check verifies RetroArch import,
physical audio/controller devices, native ARM performance, ARMv7 runtime, or a
16 KB-page device. Those still need testing. BIOS and games are not distributed
with the core or the test frontend.

