# GP32emu

GP32emu emulates the Game Park GP32, the Korean handheld released in 2001. It
provides Windows and Android applications and a libretro core for RetroArch. This fork of
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
- Shortens silent card-loading pauses such as Blue Angelo's dialogue opening.
- Fixes the click noise in Astonishia Story R's music. The correction runs in
  memory while the game plays, and the card image is not changed.

## Package contents

| ZIP package | Binaries at the archive root |
| --- | --- |
| `gp32emu-1.0.0-windows-x64.zip` | `gp32emu_win64.exe`, `SDL3.dll`, `SDL3-LICENSE.txt` |
| `gp32emu-1.0.0-libretro-windows-x64.zip` | `gp32emu_libretro.dll`, `gp32emu_libretro.info` |
| `gp32emu-1.0.0-linux-aarch64.zip` | `gp32emu_libretro.so`, `gp32emu_libretro.info` |
| `gp32emu-1.0.0-android-arm64.zip` | `gp32emu_libretro_android.so`, `gp32emu_libretro.info` |
| `gp32emu-1.0.0-android-armv7.zip` | `gp32emu_libretro_android.so`, `gp32emu_libretro.info` |

The separate `gp32emu-1.0.0-android.apk` installs the Android application; it is
not a ZIP archive and does not require RetroArch. It supports Android 6.0 or
newer and includes ARM64 and ARMv7. ARM64 is recommended.

The Linux core targets AArch64 with glibc 2.17 or newer. The Android RetroArch
cores target API 21 or newer. ARMv7 uses the interpreter and is substantially
slower than ARM64. Physical Android devices and Android RetroArch integration
still need runtime testing.

Each ZIP also contains this README, release notes, the
[Korean manual](docs/MANUAL.ko.md), third-party notices in [licenses/](licenses/),
and `manifest.json` with checksums for the packaged files. BIOS images, games and
save files are not included.

## Windows application

1. Extract the package and keep `SDL3.dll` beside `gp32emu_win64.exe`.
2. Run the executable. On first start it offers to select your GP32 BIOS; you can
   change it later with `Settings > Set BIOS...`.
3. Open a game with `File > Open Game...`, `File > Game Library...` or
   `File > Recent Games`, or drop an SMC, FXE or FPK file on the window.

Settings, states, screenshots and recordings are kept beside the executable
(`GP32emu.ini` and the `states`, `screenshots` and `recordings` folders), so
use a writable folder. A BIOS is recommended for commercial games. The game
library lists SMC, FXE and FPK files directly inside one registered folder,
excluding save files; extract ZIP archives first. Menus follow the Windows display
language (Korean or English); set `Language=ko` or `Language=en` under `[UI]` in
`GP32emu.ini` to choose one.

| Key | Action |
| --- | --- |
| Arrow keys | D-pad |
| Z / X | A / B |
| A / S | L / R |
| Enter / Shift | Start / Select |
| F5 / F8 | Save / load the state in the current slot |
| F6 / F7 | Previous / next state slot (1-9) |
| F9 | Pause or resume |
| Tab (hold) | Fast forward, with sound muted |
| F11 or Alt+Enter | Fullscreen; Esc leaves fullscreen |
| F12 | Screenshot into the `screenshots` folder |

`Settings > Keyboard Controls...` remaps the GP32 buttons. Esc, Tab, Alt, the
Windows keys and F5-F12 stay reserved for the shortcuts above. Gamepads are
detected when plugged in: the D-pad or left stick moves, the bottom and right
face buttons are A and B, the shoulder buttons or triggers are L and R, and
Start and Back/View are Start and Select. `Help > Controls and Shortcuts` shows
the current keys. Esc does not close the application; use `File > Exit`, the
close button or Alt+F4.

## Android application

Install the APK from the release page. On the start screen, choose your GP32
BIOS, then **Add game** to import an extracted `.smc`, `.fxe` or `.fpk` file.
Imported games appear in the game list; the most recent one has a **Continue**
button. Touch and hold a game to remove its copy and saves. The app follows the
system language (Korean or English).

During play, Back or the ≡ button pauses and opens the menu: resume, save state,
load state, restart, show or hide touch controls, touch vibration, sharp integer
scaling, game list, about and quit. Each game has three state slots. Touch
controls cover the D-pad, A/B, L/R, Start and Select, and
move beside the picture in landscape. Multiple buttons can be held together.
Rotation, focus loss and backgrounding release held input.

Keyboard controls match the Windows defaults. Gamepads work with touch controls
hidden; the D-pad or left stick, A/B, L1/R1 or L2/R2, Start and Select map to the
GP32. A gamepad's Mode/Menu button, or Start and Select pressed together, opens
the menu. Imported files, in-game saves and states are kept in
private app storage; original files are unchanged. Uninstalling the app removes
these copies and saves. If playback stops with an internal error, the app returns
to the game list. The APK uses an existing development signing key for
sideloading; it is not a Play Store release.

## RetroArch

1. Install the core matching your platform and RetroArch architecture. Put its
   `.info` file in RetroArch's configured core information directory.
2. Put your GP32 BIOS in RetroArch's system directory, preferably as
   `gp32166m.bin`. Any GP32 BIOS dump of 512 KiB or less works; other names such
   as `gp32166.bin`, `gp32.bin`, `GP32.BIN`, `bios.bin` and the usual
   `[BIOS] GamePark GP32 ...` dumps are recognized too. A BIOS is recommended;
   booting without one is limited.
3. Load the GP32emu core, then a `.smc`, `.fxe` or `.fpk` file. Use RetroPad port 1.

Leave boot mode on `auto` for normal use. See the [manual](docs/MANUAL.ko.md)
for controls, options and installation details.

## Saves

For an unpacked `.smc` file, the original card image is never modified. Ordinary
card accesses during play behave normally. Windows and RetroArch save changed
card pages automatically, at most once per 10 seconds after card writes have
been idle for 2 seconds, and flush again when the game is paused, closed or
replaced. Builds with worker threads enabled (the default) write in the
background to keep storage I/O off the emulation thread. Saves are staged and
flushed before replacing the previous file;
failed attempts retain pending changes and retry. Windows uses
`<original filename including .smc>.gp32.sav` next
to the ROM, for example `game.smc.gp32.sav`. RetroArch uses `<stem>.gp32.sav`
in its configured save directory. The save location must be writable, and the
delta must be loaded atop the exact original `.smc`. Extract ZIP archives
before loading them. If RetroArch supplies no save directory, the system
directory is used. Games with the same filename stem share a save location;
keep their saves in separate directories.

A save file that holds a full card image, either as `.gp32.smc` or as
`.gp32.sav`, is loaded as it is. The first successfully written page-delta save
takes its place; if the old file cannot be removed it is simply left behind.
Invalid saves or deltas for a different original stop loading instead of being
overwritten. Keep backups of valuable progress.

Use a BIOS for in-game saves in commercial SMC games. Where direct boot does not
preserve card writes, use save states instead.
If saving fails, check free space and write access. An unreadable saved card
stops loading rather than being silently replaced.

Save states are separate snapshots of the running machine. They may contain
base-relative card deltas, so preserve the exact original ROM. Restoring a state
also rolls back in-game card progress.

## Limitations

- Some cards need a BIOS. Direct boot is limited for titles including Astonishia
  Story R, Hany and Super Plusha.
- The Astonishia Story R audio-click fix and the Pinball Dreams startup-mute fix
  are on by default and leave the ROM unchanged. In RetroArch, keep
  `Fix game code bugs` enabled; the Windows application always applies them.
- Audio fidelity and input latency have not been measured against a physical
  GP32.
- Compatibility checks cover selected scenes, not complete playthroughs.
- Rewind and run-ahead in a real RetroArch session are unverified.
- Android app and core smoke checks have passed in an Android 11 AVD. Native
  ARM device performance and Android RetroArch integration remain unverified.
  Qt runtime and browser playback are also unverified; these frontends are
  available in source form.

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
zip library, the public-domain miniz library (parts of which carry the RAD Game
Tools, Valve Software and Rich Geldreich notices) and parts of BDMEmu's
frontend. Their notices are in [licenses/](licenses/).

The Windows package includes SDL3 under the zlib licence, with its notice in
`SDL3-LICENSE.txt`. In the source tree, SDL3's `LICENSE.txt`
is inside `third_party/sdl/SDL3-3.4.10.tar.gz`.

Game ROMs and BIOS files are not distributed with the emulator; supply your own dumps.
