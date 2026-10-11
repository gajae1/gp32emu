# GP32emu 1.0.1

## Changes

- Adds an experimental Nintendo Switch NRO core with ARM64 JIT support.

- Fixes stale instruction reuse after DMA and file loading replace code in RAM,
  including DMA transfers that begin outside RAM. Data-only transfers avoid
  unnecessary cache checks.
- Adds separate keyboard and gamepad button mapping to the Android app. Open
  **Controls** from the game list or pause menu, select a control, then press a
  key. Duplicate assignments swap; edits can be saved, cancelled or reset.
- Asks before restarting an Android game to avoid losing unsaved progress.
- Updates the installation, controls, save-file and upgrade instructions for each
  platform.

## Downloads and upgrading

| Package | Use |
| --- | --- |
| `gp32emu-1.0.1-windows-x64.zip` | Windows application |
| `gp32emu-1.0.1-libretro-windows-x64.zip` | Windows RetroArch core |
| `gp32emu-1.0.1-linux-aarch64.zip` | 64-bit ARM Linux RetroArch core |
| `gp32emu-1.0.1-android.apk` | Android application; RetroArch is not required |
| `gp32emu-1.0.1-android-arm64.zip` | Android ARM64 RetroArch core |
| `gp32emu-1.0.1-android-armv7.zip` | Android ARMv7 RetroArch core |
| `gp32emu-1.0.1-switch-libnx.zip` | Experimental Nintendo Switch RetroArch NRO core |
| `gp32emu-1.0.1-switch-libnx-source.zip` | Switch core source, compiled static library and build instructions |
| `gp32emu-1.0.1-source.zip` | Full project source for all platforms |

Android users can install the APK over 1.0.0 to keep imported games and saves.
Do not uninstall the old app first. On Windows, close the emulator before
replacing its program files and keep your INI, saves and state folders.
For RetroArch, replace the core and information file for the same architecture.
Version 1.0.0 downloads remain available.

The app requires Android 6.0 or newer. Android cores require API 21 or newer;
ARMv7 runs without a native JIT. The Linux core requires 64-bit AArch64 Linux,
glibc 2.17 or newer, and a compatible 64-bit libretro frontend.

The Switch NRO includes RetroArch 1.22.2. Extract its `retroarch` folder onto
the SD card alongside an existing Switch RetroArch installation. The build
is experimental and has not been tested on a console. ARM64 JIT is enabled
when the homebrew environment permits it, with an interpreter fallback.
See the included Switch guide for installation and reporting test results.

Supply your own BIOS and games. See the README and manual for installation,
controls, saves and compatibility notes.
