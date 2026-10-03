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

Use the [Korean library inventory guide](../../docs/KOREAN_LIBRARY.md) to
check supplied files and deduplicate ZIP/plain copies before adding games.
The region label alone does not verify the game's displayed language.

The catalogued Korean releases still absent from the supplied local assets are
Funny Soccer 2002, Holeman Battle Race 2002, Story of Bug eyed Monster, Winter Is,
Tales of Windy Land, and Tears - Another Story. Supply the matching owned dumps
to add them; the packaging does not fetch game content.

For OS builders using the tested RetroArch 1.22.2 revision, see the optional
[ALSA wait correction and GP32 frontend selection](retroarch-patches/README.md).
The tested H700 now selects a separately rebuilt frontend only for the GP32
core; other systems retain their original frontend. The package contains source
patches and instructions, not a replacement RetroArch binary. This is not a
verified fix for all gameplay frame pacing.

## H700 GP32 pacing profile

`h700/gp32emu.cfg` uses RetroArch's existing non-threaded video path and raises
the audio rate-control correction bound from 0.5% to 2%. It does not force a
2% pitch/speed change: RetroArch adjusts its resampling ratio from FIFO fill.
The larger bound lets audio follow the tested output cadence without exhausting
the software FIFO. Vsync and the wakeup-corrected `alsathread` frontend are
prerequisites; the profile does not change global settings, governor or volume.

On the tested installation, the core override belongs at
`RetroArch/.retroarch/config/gp32emu/gp32emu.cfg`. Use the device's configured
`rgui_config_directory` if different. If an override already exists, preserve
its other entries and review these two values rather than replacing the file.
Game/content overrides may take precedence. Removing the newly added override
restores the inherited settings when no prior override existed.

In isolated H700 replays, Her Knights Korea's last 1,200 frame intervals had
p99 17.135 ms, maximum 17.289 ms and none over 20 ms. Tomak Korea's last 600
had p99 17.219 ms and one interval over 20 ms (20.134 ms). All sampled late
clocks were 1,512 MHz. Neither run explicitly padded its central audio window
(37/13 seconds respectively); source state/audio counters and final screenshots
matched their references, with all audio accepted and no ALSA errors.

With synchronous video, the full `retro_run` duration bounds the software
core-start-to-driver-return delay: its p99 was 16.639 ms for Her Knights and
16.715 ms for Tomak. This is not physical button-to-photon latency. The output
cadence was about 59.57 frames/s, not proof of a physical panel rate or a
universal 60-fps guarantee. A custom current-frame wait achieved similar results
but was discarded in favor of the existing RetroArch path. Longer sessions,
other games and heavy shaders still need acceptance checks.

Evidence: `F:/GP32/results/resume43-rate/{g1-integrity.json,
g2-presentation-summary.json,n1-native-summary.json,n1-integrity.json,
t1-integrity.json,restored.json}`. Diagnostic binaries remain separate from
the production frontend/core.

The two-entry override is installed on the tested H700. Its existing production
frontend loaded the core override automatically and completed another Her
Knights replay (late interval p99 17.148 ms, maximum 17.825 ms). A separate
Tomak integration run loaded the installed file, displayed the Korean Spruce
in-game menu, resumed gameplay and returned to MainUI. This menu run is not a
performance comparison: pausing reduces its completed emulation-frame count.
The pre-existing global config, core options, core/frontend binaries and game
files are retained. Installation and integration evidence:
`F:/GP32/results/resume43-rate/{installed-profile.json,installed-menu.json,
p2-menu.png}` and `resume43-rate-p2-runtime-verified.json`.
