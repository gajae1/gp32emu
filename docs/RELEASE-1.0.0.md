# GP32emu 1.0.0

The first release of this GP32emu fork provides a Windows application and
RetroArch cores for emulating the Game Park GP32.

## Features

- Loads `.smc` cards and `.fxe` / `.fpk` homebrew, with BIOS boot and limited
  booting without a BIOS.
- Provides x86-64 and AArch64 JIT execution, save states, and persistent
  SmartMedia saves when a writable card is mounted.
- Offers LCD persistence and frame interpolation, plus guest CPU speed options
  in RetroArch. Windows includes keyboard and gamepad input, fullscreen,
  BMP screenshots and MKV recording.

## Files

| ZIP package | Binaries at the archive root |
| --- | --- |
| `gp32emu-1.0.0-windows-x64.zip` | `gp32emu_win64.exe`, `SDL3.dll`, `SDL3-LICENSE.txt` |
| `gp32emu-1.0.0-libretro-windows-x64.zip` | `gp32emu_libretro.dll`, `gp32emu_libretro.info` |
| `gp32emu-1.0.0-linux-aarch64.zip` | `gp32emu_libretro.so`, `gp32emu_libretro.info` |
| `gp32emu-1.0.0-android-arm64.zip` | `gp32emu_libretro_android.so`, `gp32emu_libretro.info` |
| `gp32emu-1.0.0-android-armv7.zip` | `gp32emu_libretro_android.so`, `gp32emu_libretro.info` |

Linux requires AArch64 and glibc 2.17 or newer. Android cores target API 21 or
newer. Every ZIP also contains the README, these notes, `docs/MANUAL.ko.md`,
`licenses/` and `manifest.json`. BIOS images, games and saves are not included.

## Installation

For Windows, extract the package, keep `SDL3.dll` beside the executable, select
your BIOS through `Config > Set BIOS path...`, and open a game from `File`.
For RetroArch, install the core matching the app's architecture and place its
`.info` file in the configured core information directory. Put your BIOS 1.6.6
dump in the system directory as `gp32166m.bin`, then load the core and content.

A BIOS is recommended. See the README and Korean manual for controls, save-file
locations and the limits of direct boot. Original `.smc` files are never modified;
keep them unchanged because persistent saves and states are restored relative to
the exact original card.

## Known limitations

Some cards need a BIOS, and Pinball Dreams can be silent with direct boot.
Astonishia Story R's title music retains artifacts from the game's own decoder. Compatibility checks
cover selected scenes, not complete playthroughs; audio fidelity and input
latency have not been measured against a physical GP32. RetroArch rewind and
run-ahead are unverified. Android cores have been compiled but runtime use is
unverified; ARMv7 uses the slower interpreter.

GP32emu originates with gameblabla. See the README for upstream credits and the
existing licensing status, `licenses/` for third-party notices, and the Windows
ZIP's `SDL3-LICENSE.txt` for SDL3's zlib licence.
