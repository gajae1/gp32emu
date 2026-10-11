# GP32 on Spruce

`Emu/GP32` is a system entry for Spruce's normal `standard_launch.sh` flow. `RetroArch/.retroarch/info`
holds the core metadata. Install an AArch64 Linux `gp32emu_libretro.so` next to `config.json` in `Emu/GP32`.
Keep any settings you already changed when you update an existing GP32 entry.

Put your own games in `Roms/GP32` and `gp32166m.bin` in the BIOS folder. Supported extensions are
`smc`, `fxe` and `fpk`. RetroArch also opens a `zip` holding one of them, but extract SMC images
before playing so persistent card saves can be restored against the original file. No games,
BIOS or saves are included here.

The entry uses Spruce's stock 64-bit RetroArch (`ra64.h700`), so the normal menu, Game Switcher and
button handling keep working. The entry does not replace RetroArch or its global settings.
The GP32 item starts in Spruce's Smart CPU mode; the volume is left unchanged.

Start with your existing frontend settings. `h700/gp32emu.cfg` is an optional H700 tuning example,
not a required installation step. It turns off threaded video and sets the audio rate-control
bound to 2%. Its effect depends on the frontend and device; it does not guarantee uninterrupted
audio or correct noise produced by a game.

If you choose to use it, the per-core override location is
`RetroArch/.retroarch/config/gp32emu/gp32emu.cfg`. Merge the two settings into any existing file
instead of replacing your other settings.

`gp32.png` is 120 x 130 pixels, the size of the neighbouring system tiles on a 720 x 480 menu. MainUI
caches textures, so restart it after replacing the image.

A Spruce build with `standard_launch.sh`, per-system core lookup and the 64-bit H700 RetroArch is
required. Other firmware versions may need a different launcher.

