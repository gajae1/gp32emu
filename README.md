# GP32emu

GP32emu emulates the Game Park GP32, the Korean handheld released in 2001. It
provides a Windows application and a libretro core for RetroArch. This fork of
[gameblabla's gp32emu](https://github.com/gameblabla/gp32emu) focuses on performance,
including on lower-powered ARM devices, and is maintained at
[gajae1/gp32emu](https://github.com/gajae1/gp32emu).

Supply your own BIOS and game dumps; neither is included.

[Downloads](https://github.com/gajae1/gp32emu/releases) · [한국어 사용설명서](docs/MANUAL.ko.md)

## Features

- Loads `.smc` SmartMedia images and `.fxe` / `.fpk` homebrew.
- Supports GP32 BIOS boot and limited booting without a BIOS.
- Uses a dynamic recompiler (JIT) on x86-64 and little-endian AArch64, with a
  portable interpreter for other hosts.
- Supports save states and in-game SmartMedia saves when a writable card is mounted.
- Offers optional LCD persistence and frame interpolation. RetroArch also offers
  guest CPU speed settings from 100% to 300%.

## Package contents

| Folder | Files |
| --- | --- |
| `windows-x64/` | `gp32emu_win64.exe`, `SDL3.dll`, `SDL3-LICENSE.txt`, `gp32emu_libretro.dll`, `gp32emu_libretro.info` |
| `linux-aarch64/` | `gp32emu_libretro.so`, `gp32emu_libretro.info` |
| `android-arm64/` | `gp32emu_libretro_android.so`, `gp32emu_libretro.info` |
| `android-armv7/` | `gp32emu_libretro_android.so`, `gp32emu_libretro.info` |

The Linux core targets AArch64 with glibc 2.17 or newer. The Android cores target
Android API 21 or newer; they have been compiled, but Android runtime use is
unverified. ARMv7 uses the interpreter and is substantially slower than ARM64.

The package also contains this README, release notes, the
[Korean manual](docs/MANUAL.ko.md), third-party notices in [licenses/](licenses/),
and `manifest.json` with checksums for the packaged files. BIOS images, games and
save files are not included.

## Windows application

1. Extract the package and keep `SDL3.dll` beside `gp32emu_win64.exe`.
2. Run the executable. Use `Config > Set BIOS path...` to select your BIOS.
3. Use `File > Open SmartMedia image...`, `Open FXE...` or `Open FPK...` to open a game.

The BIOS path and settings are stored in `GP32emu.ini` beside the executable, so
use a writable folder. A BIOS is recommended for commercial games.

Use the arrow keys for directions, **Z/X** for GP32 **A/B**, **A/S** for **L/R**,
**Enter** for **Start**, and **Shift** for **Select**. **F5/F8** open save-state
save/load dialogs; **F12** opens a BMP screenshot save dialog. **F11** or
**Alt+Enter** toggles fullscreen. **Esc** leaves fullscreen, or closes the
application when already windowed.

## RetroArch

1. Install the core matching your platform and RetroArch architecture. Put its
   `.info` file in RetroArch's configured core information directory.
2. Put your GP32 BIOS 1.6.6 dump in RetroArch's system directory as `gp32166m.bin`.
   A BIOS is recommended; booting without one is limited.
3. Load the GP32emu core, then a `.smc`, `.fxe` or `.fpk` file. Use RetroPad port 1.

Leave boot mode on `auto` for normal use. See the [manual](docs/MANUAL.ko.md)
for controls, options and installation details.

## Saves

Windows BIOS-booted SMC games keep in-game saves in `game.smc.gp32.smc` beside
`game.smc`. RetroArch uses `<game>.gp32.smc` in its save directory, without the
original extension; if no save directory is supplied, the core uses its system
directory. Saves are written when content is closed or replaced, and restored
on the next load. The original card file is left unchanged. In RetroArch, games
with the same filename stem share a save in the same save directory.

Use a BIOS for in-game saves in commercial SMC games. Where direct boot does not
preserve card writes, use save states instead.
If saving fails, check free space and write access. An unreadable saved card
stops loading rather than being silently replaced.

Save states are separate from in-game saves. Keep the original card file
unchanged and load the same content before restoring a state. Restoring a state
can also return the card's in-game progress to an earlier point.

## Limitations

- Some cards need a BIOS. Direct boot is limited for titles including Astonishia
  Story R, Hany and Super Plusha. Pinball Dreams can be silent on the direct-boot path.
- Astonishia Story R's title music retains artifacts from the game's own audio
  decoder. Audio fidelity and input latency have not been measured against a
  physical GP32.
- Compatibility checks cover selected scenes, not complete playthroughs.
- Rewind and run-ahead in a real RetroArch session are unverified.
- Android runtime use is unverified. Qt runtime and browser playback are also
  unverified; these frontends are available in source form.

## Building from source

See [BUILDING.md](https://github.com/gajae1/gp32emu/blob/main/docs/BUILDING.md) for
build commands, source-only frontends and the source layout.

## Credits and licensing

GP32emu was started by [gameblabla](https://github.com/gameblabla/gp32emu). This
fork keeps the upstream commits in its history. The upstream repository does not
state a licence for its own code at the time of writing, so this repository does
not add one either; ask the upstream author before reusing it outside GitHub's
fork terms.

Parts of the hardware model come from MAME (BSD-3-Clause, Tim Schuerewegen,
Raphael Nabet and others). The repository also includes the MIT-licensed kuba--
zip library and parts of BDMEmu's frontend. Their notices are in
[licenses/](licenses/).

The Windows package includes SDL3 under the zlib licence, with its notice beside
the DLL as `windows-x64/SDL3-LICENSE.txt`. In the source tree, SDL3's `LICENSE.txt`
is inside `third_party/sdl/SDL3-3.4.10.tar.gz`.

Game ROMs and BIOS files are not distributed with the emulator; supply your own dumps.
