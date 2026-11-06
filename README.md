# GP32emu

GP32emu runs Game Park GP32 games on Windows, Android, iPhone/iPad and
compatible RetroArch frontends. It opens `.smc` card images and `.fxe`/`.fpk`
homebrew. Bring your own BIOS and games; neither is included.

[Downloads](https://github.com/gajae1/gp32emu/releases) ·
[한국어 사용설명서](docs/MANUAL.ko.md) ·
[1.1.0 update notes](docs/RELEASE-1.1.0.md)

**1.0.1** is the current public release. The iPhone/iPad app is part of the
unpublished **1.1.0** candidate.

## Get started

| Where to play | What to install |
| --- | --- |
| Windows | Extract the Windows app ZIP. Keep `SDL3.dll` beside `gp32emu_win64.exe`, run it and open a game. |
| Android | Install the APK, then use **Add game** to import an extracted game file. |
| iPhone/iPad | Sign and install the unsigned IPA using the [iPhone/iPad guide](ios/README.md). |
| RetroArch | Install the core and its `.info` file for your platform and architecture, then load a game. |
| Nintendo Switch | Follow the [Switch installation guide](https://github.com/gajae1/gp32emu/blob/main/packaging/switch/README.md). |

A BIOS is recommended for commercial games. Select it in the app, or put
`gp32166m.bin` in RetroArch's system directory and leave **Boot mode** on
`auto`. iOS and Switch support is experimental.

## Updates and saves

Back up your in-game saves and save states before updating. Keep the original
`.smc` alongside any saves that depend on it. Update mobile apps over the
existing installation using a matching app identifier and signature.

GP32emu 1.1.0 writes v17 save states, which older builds cannot read. Loading a
state also restores the in-game card progress from that moment. See
[saves and backups](docs/MANUAL.ko.md#저장과-백업) for file locations and compatibility.

## Credits and licensing

This fork is based on [gameblabla's gp32emu](https://github.com/gameblabla/gp32emu).
The upstream repository does not state a licence for its own code; this fork
does not add one. See [third-party notices](licenses/) for MAME-derived code,
the ZIP libraries and BDMEmu frontend code. The Windows ZIP includes the SDL3
licence as `SDL3-LICENSE.txt`.

[Build from source](https://github.com/gajae1/gp32emu/blob/main/docs/BUILDING.md)
