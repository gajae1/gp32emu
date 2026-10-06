# GP32emu 사용설명서

RetroArch용 GP32 코어를 쓰는 방법을 적은 문서입니다. 빌드 방법은 README.md에 있습니다.

## 필요한 것

- 코어 파일: `gp32emu_libretro.so`(리눅스, 안드로이드는 `_android.so`) 또는 `gp32emu_libretro.dll`(윈도우)
- GP32 BIOS 1.6.6: 파일 이름을 `gp32166m.bin`으로 바꿔서 사용합니다.
- 게임 파일: `.smc`, `.fxe`, `.fpk`

BIOS와 게임은 이 저장소에 들어 있지 않습니다. 직접 갖고 있는 덤프를 쓰세요.

## RetroArch(PC)

1. 코어 파일을 RetroArch의 `cores` 폴더에 넣습니다.
2. `gp32166m.bin`을 `system` 폴더에 넣습니다.
3. RetroArch에서 코어 불러오기로 GP32emu를 고르고, 콘텐츠 불러오기로 게임 파일을 엽니다.

명령줄로는 `retroarch -L gp32emu_libretro.so 게임.smc` 입니다. 코어 경로에 슬래시가 없으면 RetroArch가
코어 이름으로 읽으니, 현재 폴더의 파일은 `./gp32emu_libretro.so`처럼 적어야 합니다.

## SpruceOS(H700 기기: RG35XX SP 등)

1. SD카드의 `Emu` 폴더에 `packaging/spruce/Emu/GP32` 폴더를 통째로 복사합니다.
2. 그 안에 H700(aarch64)용 `gp32emu_libretro.so`를 넣습니다.
3. `packaging/spruce/RetroArch/.retroarch/info/gp32emu_libretro.info`를 SD카드의 같은 경로에 복사합니다.
   코어 옆에 info 파일만 두면 RetroArch가 읽지 않습니다.
4. `gp32166m.bin`을 SD카드의 `BIOS` 폴더에 넣습니다.
5. 게임은 `Roms/GP32`에 넣습니다. 메뉴에서 목록을 새로고침하면 GP32 항목이 나타납니다.

설치할 때 기존 RetroArch 파일, Spruce 스크립트, 설정 파일은 건드리지 않습니다. 이미 GP32 항목을
설정해 둔 상태라면 `config.json`을 덮어쓰지 말고 코어 파일만 교체하세요.
CPU 모드는 기본이 Smart입니다. 무거운 장면에서 소리가 끊기면 게임 메뉴에서 Performance로 바꿔 보세요.

## 조작

RetroPad 1번 포트를 그대로 씁니다.

| RetroPad | GP32 |
| --- | --- |
| 방향키 | 방향키 |
| A, B | A, B |
| L, R | L, R |
| Start | Start |
| Select | Select |

버튼 배치가 마음에 안 들면 RetroArch의 입력 리맵 기능으로 바꾸면 됩니다.

## 코어 옵션

RetroArch 빠른 메뉴의 코어 옵션에서 바꿉니다.

- **Dynamic recompiler**: 기본값은 켜짐(JIT). 저사양 ARM 기기에서는 켜져 있어야 제 속도가 납니다.
  문제를 의심할 때만 끄고 비교해 보세요.
- **Boot mode**: `auto`가 기본입니다. BIOS가 있으면 BIOS로 부팅하고, 없으면 카드에서 바로 부팅합니다.
  `require_bios`는 BIOS가 없을 때 오류를 내고, `direct_hle`는 BIOS가 있어도 쓰지 않습니다.
- **LCD persistence**: 원래 액정의 잔상을 흉내 냅니다. 기본값은 꺼짐.
- **Frame interpolation**: 프레임 사이를 섞어 줍니다. 기본값은 꺼짐.
- **CPU speed**: 100%에서 300%까지. 게임에 주는 CPU 시간만 늘리고 소리 높이, 타이머, 화면 갱신은 그대로 둡니다.
  원래 기기에서도 느렸던 게임이나 로딩이 긴 게임에 도움이 되지만, 그만큼 기기 CPU를 더 씁니다.
  저사양 기기에서는 필요한 게임에서만 올리세요. 올리면 일부 게임이 이상하게 동작할 수 있습니다.

## 저장

- **게임 안 저장**: GP32 게임은 SmartMedia 카드에 저장합니다. 이 내용은 RetroArch의 저장 폴더에
  `게임이름.gp32.smc`로 만들어집니다. 게임을 닫을 때 기록됩니다. 같은 이름의 게임 파일은 같은
  저장 파일을 공유합니다.
- **세이브 스테이트**: RetroArch의 보통 방식대로 저장하고 불러옵니다. 한 슬롯이 10MB가량입니다.
- 되감기와 런어헤드를 써도 소리와 화면이 처음 진행과 같게 나오도록 맞춰 두었습니다.
  이전 버전에서 만든 스테이트도 불러올 수 있습니다.

## 문제가 생길 때

- **BIOS 화면(DATA LOADING)에서 멈춤**: 상용 게임이 아닌 자작 카드(GPMM 구조)는 BIOS가 시작하지 못합니다.
  Boot mode가 `auto`이면 자동으로 바로 부팅하니 `require_bios`로 바뀌어 있지 않은지 보세요.
- **BIOS 없이 켰더니 시작 화면에서 멈춤**: BIOS 없이 바로 부팅하는 방식은 아직 모든 카드에서 되지는
  않습니다. BIOS를 넣고 쓰세요.
- **소리가 가끔 끊김**: 기기 CPU 모드를 Performance로 바꾸고, 같은 CPU를 쓰는 다른 프로그램을 끄세요.
  무거운 로딩 구간에서 순간적으로 느려져 끊기는 경우가 있습니다.
- **Pinball Dreams의 소리가 없음**: 알려진 문제입니다. 게임이 스스로 볼륨을 최소로 설정합니다.
- **RetroArch가 바로 꺼짐**: `retroarch --verbose`로 실행해서 로그를 확인하세요.

## 브라우저에서 해 보기

`make -f Makefile.wasm`으로 `web/gp32_wasm_core.wasm`을 만든 뒤 `make -f Makefile.wasm serve`를 실행하고
`http://127.0.0.1:8008/`을 엽니다. BIOS와 게임 파일을 페이지에 끌어다 놓으면 됩니다.
브라우저 빌드는 JIT 없이 인터프리터로만 돌기 때문에 PC 정도의 성능이 필요합니다.

