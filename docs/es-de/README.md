# Play GP32 games in ES-DE

**한국어 설치** · RetroArch에서 GP32emu 코어로 게임이 열리는지 확인하세요.
아래 표에서 운영체제에 맞는 `es_systems.xml`을 받아 ES-DE 데이터 폴더의
`custom_systems`에 넣으세요. 기존 파일이 있으면 `<systemList>` 안에 GP32
항목만 합치세요. ROM 폴더 아래 `gp32`에 압축을 푼 게임을 넣고 ES-DE를 다시 시작하세요.

1. Install RetroArch and the GP32emu core for your platform. In RetroArch,
   open a game once with **Load Core** and **Load Content**. For commercial
   games, put your BIOS in the RetroArch system directory as `gp32166m.bin`.
2. Choose the `es_systems.xml` for your copy of ES-DE.

   | ES-DE platform | Configuration | RetroArch core |
   | --- | --- | --- |
   | Windows | [windows/es_systems.xml](windows/es_systems.xml) | `gp32emu_libretro.dll` |
   | Linux | [linux/es_systems.xml](linux/es_systems.xml) | `gp32emu_libretro.so` |
   | Android | [android/es_systems.xml](android/es_systems.xml) | `gp32emu_libretro_android.so` |
   | macOS | [macos/es_systems.xml](macos/es_systems.xml) | `gp32emu_libretro.dylib` (build from source) |

3. Close ES-DE. In its **application data directory**, open `custom_systems`.
   If there is no `es_systems.xml`, copy the chosen file there. If there is
   one, copy just the GP32 `<system>…</system>` entry inside its existing
   `<systemList>`, replacing any previous `gp32` entry.
4. Put extracted `.smc`, `.fxe` or `.fpk` files in the **`gp32`** folder
   under your ES-DE ROM directory. Restart ES-DE and select **Game Park GP32**.

On Android, install the core in RetroArch using **Load Core → Install or
Restore a Core** and grant RetroArch access to the ROM directory. The launcher
opens RetroArch with its own controls, BIOS and saves.

If the system is missing, check the ROM folder and extension. If ES-DE reports
a missing core, check RetroArch's **Settings → Directory → Cores** and the
filename in the table. The GP32 entry skips scraping because ES-DE has no GP32
scraper platform. You can still supply your own media and gamelist.

[ES-DE user guide](https://gitlab.com/es-de/emulationstation-de/-/blob/master/USERGUIDE.md#game-system-customizations)
