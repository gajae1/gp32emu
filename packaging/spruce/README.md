# GP32 on Spruce

The `Emu/GP32` directory provides a system entry for Spruce's normal
`standard_launch.sh` flow and the `RetroArch` directory provides core metadata.
Install a separately built AArch64 Linux `gp32emu_libretro.so` alongside
`config.json` in `Emu/GP32`, as used by the tested Spruce launcher.
Keep existing per-device settings when updating an
already configured GP32 entry.

Put your own GP32 games in `Roms/GP32` and `gp32166m.bin` in RetroArch's system
directory. No games, BIOS, or saved game images are included in this repository.
Supported extensions are `smc`, `fxe`, `fpk`, and `zip`.

`gp32.png` is a generated illustration exported at **120 x 130 pixels**, matching
the neighboring system tiles on the tested 720 x 480 Spruce menu. Both `icon` and
`iconsel` use this filename. MainUI caches textures; after replacing an image,
exit the menu normally or restart it through Spruce so the new dimensions load.
Do not install the full-resolution generation source as a system icon.

The tested Korean-only device library has 20 unique locally supplied releases.
Existing European releases were preserved outside `Roms/GP32`; game saves and
RetroArch settings were retained. This library inventory is not a claim that
every GP32 release is present or that every game is fully compatible.

The catalogued Korean releases still absent from the supplied local assets are
Funny Soccer 2002, Holeman Battle Race 2002, Story of Bug eyed Monster, Winter Is,
Tales of Windy Land, and Tears - Another Story. Supply the matching owned dumps
to add them; the packaging does not fetch game content.
