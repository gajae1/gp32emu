# Android libretro core

Use an installed Android NDK with a C23-capable Clang, CMake and Ninja on PATH.
The tested toolchain is NDK r28b (28.2.13676358). From PowerShell:

```powershell
./scripts/build_android.ps1 -Ndk /path/to/android-ndk
```

The script builds `arm64-v8a` and `armeabi-v7a` into separate directories under
`build-android/` and writes their SHA-256 values to `manifest.json`.
Use `-Abi arm64-v8a`, `-OutputDirectory`, `-Jobs`, or `-ApiLevel` to override
the defaults. `ANDROID_NDK_HOME` can supply the NDK directory.

The 64-bit core includes the AArch64 JIT; the 32-bit build uses the portable CPU
path. Successful compilation does not prove Android runtime, frontend import,
audio-driver or executable-memory-policy compatibility. Those remain device
acceptance requirements. BIOS and games are not distributed with the core.
