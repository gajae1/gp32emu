# GP32 on Spruce

This package adds a GP32 entry to Spruce using its stock 64-bit RetroArch
(`ra64.h700`). It keeps the normal menu, Game Switcher and button controls.

## Install and update

1. Copy `Emu/GP32` to the SD card and place the AArch64 Linux
   `gp32emu_libretro.so` beside `config.json` in that folder.
2. Put `gp32emu_libretro.info` in `RetroArch/.retroarch/info`.
3. Put your games in `Roms/GP32` and `gp32166m.bin` in the BIOS folder.

When updating, keep your existing GP32 settings and global RetroArch configuration.

Supported extensions are
`smc`, `fxe` and `fpk`. RetroArch also opens a `zip` holding one of them, but extract SMC images
before playing so persistent card saves can be restored against the original file. No games,
BIOS or saves are included here.

The GP32 item starts in Spruce's Smart CPU mode; the volume is left unchanged.

## Optional settings

Start with your existing frontend settings. `h700/gp32emu.cfg` is an optional H700 tuning example,
not a required installation step. It turns off threaded video and sets the audio rate-control
bound to 2%. Its effect depends on the frontend and device; it does not guarantee uninterrupted
audio or correct noise produced by a game.

If you choose to use it, the per-core override location is
`RetroArch/.retroarch/config/gp32emu/gp32emu.cfg`. Merge the two settings into any existing file
instead of replacing your other settings.

If you replace the menu image, use a 120 x 130 pixel `gp32.png` and restart MainUI
to display it.

A Spruce build with `standard_launch.sh`, per-system core lookup and the 64-bit H700 RetroArch is
required. Other firmware versions may need a different launcher.

