# GP32emu for Nintendo Switch

This experimental RetroArch core is part of the **unpublished 1.1.0 candidate**;
the current public release is **1.0.1**. Compatibility varies by game and setup.
ARM64 JIT is used when the homebrew loader permits it; otherwise, the core uses
the interpreter.

## Install

1. Install the [official Switch RetroArch bundle](https://docs.libretro.com/guides/install-libnx/)
   first; this download does not include its menu assets.
2. Merge this archive's `retroarch` folder into the SD card root. The core goes
   in `retroarch/cores/gp32emu_libretro_libnx.nro`; its information file goes in
   `retroarch/cores/info/`, or your configured core information directory.
3. Keep your existing configuration. Put your GP32 BIOS in RetroArch's system
   directory as `gp32166m.bin`, then select the GP32emu core and a game.
4. Use full-memory application mode as described in the official guide.
   BIOS images, games and saves are not included.

Use the NRO from the Switch binary package. The source package's `.a` library
and Linux or Android `.so` files cannot be installed as Switch cores.

When reporting a problem, include the game, BIOS version, launch mode and
steps to reproduce it. Note whether it affects graphics, sound, input or saves.

## Source and notices

Matching GP32emu core sources, the static library and rebuild instructions are
in `gp32emu-1.1.0-switch-libnx-source.zip`. Full GP32emu project sources are in
`gp32emu-1.1.0-source.zip`.
RetroArch and dependency notices are retained in `licenses/`.
