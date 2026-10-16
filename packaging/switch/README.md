# GP32emu 1.1.0 for Nintendo Switch

This guide is the packaging template for unreleased 1.1.0 builds.
The current public downloads remain 1.0.1.

Experimental RetroArch core, built with devkitPro devkitA64/libnx and linked
with RetroArch 1.22.2. This build has not been tested on a Switch console.
ARM64 JIT is enabled when the homebrew loader permits it. If executable
memory is unavailable, the core falls back to the interpreter.

## Install

1. Install the [official Switch RetroArch bundle](https://docs.libretro.com/guides/install-libnx/)
   first; this download does not include its menu assets.
2. Merge this archive's `retroarch` folder into the SD card root. The core goes
   in `retroarch/cores/gp32emu_libretro_libnx.nro`; its information file goes in
   `retroarch/cores/info/`, or your configured core information directory.
3. Keep your existing configuration. Put your GP32 BIOS in RetroArch's system
   directory as `gp32166m.bin`, then select the GP32emu core and a game.
4. Use full-memory application mode as described in the official guide.
   BIOS images, games and saves are not included.

The NRO contains both the RetroArch frontend and the GP32emu core. The `.a`
library in the separate source package is for linking and cannot be loaded
from the console. Linux and Android `.so` files cannot be used on Switch.

When reporting a problem, include the game, BIOS version, launch mode and
steps to reproduce it. Note whether it affects graphics, sound, input or saves.

## 한국어 안내

공식 Switch RetroArch를 먼저 설치하고 이 파일의 `retroarch` 폴더를 SD 카드
최상위에 합치세요. 기존 설정과 메뉴 자산은 그대로 사용합니다. 시스템 폴더에
`gp32166m.bin`을 넣은 뒤 GP32emu 코어와 게임을 선택하세요.

실기 미검증 실험적 빌드이며 ARM64 JIT를 지원합니다. 실행 환경에서 JIT 권한을
제공하지 않으면 인터프리터로 대체합니다. 게임 이름·BIOS 버전·실행 모드와
재현 순서를 알려 주시면 호환성 확인에 도움이 됩니다.

## Source and notices

Matching GP32emu core sources, the static library and rebuild instructions are
in `gp32emu-1.1.0-switch-libnx-source.zip`. Full GP32emu project sources are in
`gp32emu-1.1.0-source.zip`. See `manifest.json` for exact revisions and hashes.
RetroArch and dependency notices are retained in `licenses/`.
