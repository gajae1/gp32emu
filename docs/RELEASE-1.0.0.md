# GP32emu 1.0.0

GP32emu for Windows and RetroArch, with x86-64 and AArch64 recompilers and
portable interpreter support. This release brings together the compatibility,
timing, input and audio work in this fork.

## Included

- Windows x64 standalone application and libretro core.
- Linux AArch64 libretro core, targeting Cortex-A53 and glibc 2.17 or newer.
- Android arm64-v8a and armeabi-v7a libretro cores (Android API 21 or newer).
- Korean manual, build instructions, SHA-256 manifest and third-party notices.

Keep `SDL3.dll` beside the Windows executable. For RetroArch, install the core
for your platform and its `.info` file using your existing frontend. The usual
BIOS filename is `gp32166m.bin` in the frontend's system directory. ROMs, BIOS
images and saves are not included. See README for the upstream source and the
existing licensing status.

## Recent changes

- Fixed host callbacks at 60 Hz, matching the time advanced by the core. Guest
  LCD timing remains independent, preventing audio drift and unintended speed
  changes when a game reprograms its display.
- Fixed full-scale overflow in the audio reconstruction filter and retained
  its coefficients across underruns to avoid rebuilding an unchanged filter.
- Delivered completed audio during long frames in libretro, Win64 and SDL.
  Libretro groups tiny deliveries into approximately two-ms batches: an ASR
  title capture needs 1,521 callbacks instead of 19,844 with identical PCM.
  This reduces callback overhead; it is not an equivalent game speedup.
- Staged host save-state and SmartMedia writes before replacing an old save.
  Failed writes leave the prior file intact. This cannot repair a failing
  storage card or guarantee survival of a sudden power loss.
- Integrated AArch64 memory-read and SDK task-scan optimizations, retaining
  the checked fallback paths and JIT/interpreter differential coverage.
- Enabled SDL3 gamepad input in CMake Qt builds and added Windows executable
  version metadata.
- Added Win64 BIOS-boot SmartMedia sidecar persistence across sessions and game changes;
  existing corrupt saves are rejected without replacement. Report libretro save
  failures to the frontend instead of silently ignoring them.
- Clear Win64 keyboard holds when entering a native menu, so releases consumed
  by the menu cannot leave guest buttons held down.

## Release checks

Windows regressions pass 31/31, with an additional focused cross-session
standalone save check after review. The Windows GUI starts and closes normally
in a silent smoke run and is a GUI-subsystem executable. JIT differential,
audio and persistence tests pass on native Cortex-A53 hardware. Short ASR,
Blue Angelo and Princess Maker 2 state replays complete there above the 60
host-step/s real-time baseline on average; ASR still has a transient frame
above 16.67 ms. These core-only windows exclude RetroArch output costs and
are not all-game performance guarantees. Android ARM64/v7 release builds pass.

## Compatibility notes

This is a usable release, not a claim of perfect emulation. Commercial-library
checks cover bounded scenes and scripted input, not complete playthroughs.
Some cards require a BIOS. Pinball Dreams can remain silent on the direct-boot
path. Remaining reported crackle, physical input latency and long sessions
still need confirmation. Android builds have not been run on an Android device;
ARMv7 has no JIT and is substantially slower. Qt runtime and browser playback
are not release-validated. See README for the detailed limitations.
