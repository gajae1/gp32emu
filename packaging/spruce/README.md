# GP32 on Spruce

`Emu/GP32` is a system entry for Spruce's normal `standard_launch.sh` flow. `RetroArch/.retroarch/info`
holds the core metadata. Install an AArch64 Linux `gp32emu_libretro.so` next to `config.json` in `Emu/GP32`.
Keep any settings you already changed when you update an existing GP32 entry.

Put your own games in `Roms/GP32` and `gp32166m.bin` in the BIOS folder. Supported extensions are
`smc`, `fxe`, `fpk` and `zip`. No games, BIOS or saves are included here.

The entry uses Spruce's stock 64-bit RetroArch (`ra64.h700`), so the normal menu, Game Switcher and
button handling keep working. Nothing in the package changes global RetroArch settings, the CPU
governor or the volume. The GP32 item starts in Spruce's Smart CPU mode.

`h700/gp32emu.cfg` is an optional per-core override. It turns off threaded video and raises the audio
rate-control bound from 0.5% to 2%, which keeps the audio buffer from running dry on a slow frame. It
belongs in `RetroArch/.retroarch/config/gp32emu/gp32emu.cfg`. If that file already exists, copy these
two lines into it instead of replacing it.

`gp32.png` is 120 x 130 pixels, the size of the neighbouring system tiles on a 720 x 480 menu. MainUI
caches textures, so restart it after replacing the image.

A Spruce build with `standard_launch.sh`, per-system core lookup and the 64-bit H700 RetroArch is
required. Other firmware versions may need a different launcher.

`docs/KOREAN_LIBRARY.md` describes how to check a folder of game files for duplicates.

