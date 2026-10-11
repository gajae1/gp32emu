# GP32emu 1.0.1

## Changes

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

Download the package for your platform. Windows application, Windows libretro,
Linux AArch64, Android ARM64 and Android ARMv7 cores have separate ZIP files.
The standalone Android app is `gp32emu-1.0.1-android.apk`; it does not require
RetroArch. Source is also available separately.

Android users can install the APK over 1.0.0 to keep imported games and saves.
Do not uninstall the old app first. On Windows, close the emulator before
replacing its program files and keep your INI, saves and state folders.
For RetroArch, replace the core and information file for the same architecture.
Version 1.0.0 downloads remain available.

The app requires Android 6.0 or newer. Android cores require API 21 or newer;
ARMv7 runs without a native JIT. The Linux core requires 64-bit AArch64 Linux,
glibc 2.17 or newer, and a compatible 64-bit libretro frontend. It is not tied
to H700 hardware. Android, 32-bit Linux and Switch need different binaries.

## Limits

Physical Android and RK3326 runtime validation remains outstanding. The
experimental Switch source path has no officially built or tested binary in
this release. BIOS images, games and saves are not included in public downloads.
See the README and manual for existing game-compatibility and licensing notes.
