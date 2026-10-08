# GP32emu 사용설명서

Windows 독립 실행 프로그램과 RetroArch용 GP32 코어의 설치·조작·저장 안내입니다.
BIOS와 게임 파일은 포함하지 않습니다. 직접 보유한 덤프를 사용하세요.

## 필요한 파일

| 대상 | ZIP 패키지와 루트의 실행 파일 |
| --- | --- |
| Windows x64 독립 실행 | `gp32emu-1.0.0-windows-x64.zip`: `gp32emu_win64.exe`, `SDL3.dll` |
| Windows x64 RetroArch | `gp32emu-1.0.0-libretro-windows-x64.zip`: `gp32emu_libretro.dll` |
| Linux AArch64 RetroArch | `gp32emu-1.0.0-linux-aarch64.zip`: `gp32emu_libretro.so` |
| Android ARM64 RetroArch | `gp32emu-1.0.0-android-arm64.zip`: `gp32emu_libretro_android.so` |
| Android ARMv7 RetroArch | `gp32emu-1.0.0-android-armv7.zip`: `gp32emu_libretro_android.so` |

각 코어 ZIP에는 `gp32emu_libretro.info`도 포함되어 있습니다. 모든 ZIP에는 README,
릴리스 노트, 이 설명서, `licenses/`, `manifest.json`이 함께 들어 있습니다.
Linux 코어는 glibc 2.17 이상, Android 코어는 API 21 이상을 대상으로 합니다.
Android는 컴파일만 확인했으며 실제 실행은 미검증입니다. ARMv7은 JIT 없이 동작하므로
ARM64보다 상당히 느립니다.

게임은 `.smc` 카드 이미지와 `.fxe`, `.fpk` 형식을 엽니다. GP32 BIOS 1.6.6 사용을
권장합니다. BIOS 없이 실행할 수 있는 게임은 제한되어 있습니다.

## Windows 독립 실행

1. 압축을 풀고 `gp32emu_win64.exe`와 `SDL3.dll`을 같은 폴더에 둡니다.
2. 프로그램을 실행한 뒤 `Config > Set BIOS path...`에서 BIOS를 지정합니다.
   `File > Open BIOS...`로 BIOS를 열 수도 있습니다.
3. `File > Open SmartMedia image...`, `Open FXE...`, `Open FPK...`로 해당 게임을 엽니다.

BIOS 경로와 설정은 실행 파일 옆의 `GP32emu.ini`에 저장하므로 쓰기 가능한 폴더에 두세요.
`File > Game library...`의 `Choose folder...`에서 게임 폴더 하나를 등록하면
목록에서 게임을 더블클릭해 실행할 수 있습니다. 해당 폴더 바로 아래의 SMC·FXE·FPK만
표시하며, 하위 폴더와 저장 이미지는 제외합니다. ZIP은 먼저 압축을 풀어 주세요.
등록한 폴더는 다음 실행에도 유지되며, 파일을 추가했다면 `Refresh`로 갱신합니다.
BIOS가 설정되지 않으면 HLE 대체 실행을 사용한다는 경고가 나옵니다. 이 경로로 시작하지
못하는 게임은 BIOS를 지정해서 실행하세요.

`Emulation > Run/Pause`로 일시정지·재개하고 `Reset`으로 다시 시작합니다.
`Video`에서 전체화면, 화면 비율, 정수 배율, 잔상 효과를 바꿀 수 있습니다.
화면 출력에 문제가 있으면 `GDI fallback`을 선택할 수 있습니다. `Audio`에서는
`waveOut`, `WASAPI shared`, `WASAPI exclusive`를 선택합니다.

### 키보드와 단축키

| 키 | 동작 |
| --- | --- |
| 방향키 | GP32 방향키 |
| Z / X | GP32 A / B |
| A / S | GP32 L / R |
| Enter | Start |
| Shift | Select |
| F5 | 세이브 스테이트 저장 대화상자 |
| F8 | 세이브 스테이트 불러오기 대화상자 |
| F12 | BMP 스크린샷 저장 대화상자 |
| F11 또는 Alt+Enter | 전체화면 전환 |
| Esc | 전체화면이면 창 모드로 전환. 프로그램을 종료하지 않음 |

위 표는 기본 배치입니다. `Config > Keyboard controls...`에서 바꿀 버튼을 클릭하고
새 키를 누른 뒤 `OK`를 선택하면 저장됩니다. 이미 쓰는 키를 지정하면 두 버튼의 키를
서로 바꿉니다. `Defaults`는 기본 배치 복원, `Cancel`은 변경 취소입니다.
키 입력 대기 중 Esc는 해당 입력을 취소합니다. Esc·Alt·Windows 키와
F5/F8/F10/F11/F12는 창 조작과 단축키용으로 예약되어 있습니다.
종료는 창 닫기 버튼, `File > Exit` 또는 Alt+F4를 사용하세요.

F5/F8은 자동 슬롯 저장·불러오기가 아닙니다. 파일 대화상자에서 경로를 선택합니다.
`File` 메뉴의 `Save State...`, `Load State...`, `Take Screenshot...`도 같은 기능입니다.
`Start Recording ZMBV MKV...`와 `Stop Recording`으로 MKV 녹화를 시작·종료할 수 있습니다.

### 명령줄

실행 파일이 있는 폴더의 PowerShell에서 다음과 같이 실행합니다.

```powershell
.\gp32emu_win64.exe --bios "gp32166m.bin" --smc "game.smc"
.\gp32emu_win64.exe --fxe "homebrew.fxe"
.\gp32emu_win64.exe --fpk "homebrew.fpk"
```

`--jit`은 JIT 켜기, `--no-jit`은 끄기, `--no-audio`는 소리 출력 끄기입니다.
옵션 없이 넘긴 파일 경로는 SMC 이미지로 취급하므로 FXE/FPK에는 해당 옵션을 쓰세요.
경로에 공백이 있으면 따옴표로 감쌉니다. `--bios`로 지정한 경로는 `GP32emu.ini`에도 저장됩니다.

## RetroArch 설치

### PC와 Linux 휴대기기

1. RetroArch의 플랫폼과 아키텍처에 맞는 코어를 설정된 코어 폴더에 넣습니다.
   Linux AArch64 코어는 x86-64 PC용이 아닙니다.
2. `gp32emu_libretro.info`를 RetroArch의 **코어 정보 파일 폴더**에 넣습니다.
   코어 폴더와 정보 파일 폴더는 다를 수 있습니다.
3. BIOS를 RetroArch의 **시스템/BIOS 폴더**에 `gp32166m.bin` 이름으로 넣습니다.
4. 코어 불러오기에서 GP32emu를 고른 뒤 콘텐츠 불러오기에서 게임을 엽니다.

휴대기기의 코어·정보·BIOS 폴더는 기기별로 다릅니다. 기존 RetroArch 설정을 덮어쓰지 말고
설정에 지정된 위치를 사용하세요. 게임 목록을 따로 관리하는 메뉴에서는 목록 새로고침이
필요할 수 있습니다.

Linux에서 현재 폴더의 코어를 명령줄로 불러오는 예입니다.

```sh
retroarch -L ./gp32emu_libretro.so "game.smc"
```

### Android

RetroArch 앱의 아키텍처에 맞춰 ARM64 또는 ARMv7 코어를 선택합니다. 기기의 운영체제가
64비트여도 앱이 32비트라면 ARMv7 코어가 필요합니다. 앱에서 제공하는 로컬 코어 설치 기능으로
`gp32emu_libretro_android.so`를 설치한 뒤, 정보 파일과 BIOS를 앱에 설정된 폴더에 넣고
콘텐츠를 불러옵니다. 코어 설치 방법과 파일 접근 권한은 RetroArch 배포판에 따라 다릅니다.
Android에서의 실행과 조작은 아직 확인되지 않았습니다.

### 조작

RetroPad 1번 포트를 사용합니다. 방향키, A/B, L/R, Start, Select는 각각 같은 이름의
GP32 버튼에 대응합니다. 실제 키보드·게임패드 배치는 RetroArch의 입력 설정과 리맵에서 바꿉니다.
위의 Windows 키보드 표는 독립 실행 프로그램에만 적용됩니다.

### 코어 옵션

RetroArch 빠른 메뉴의 코어 옵션에서 설정합니다.

| 옵션 | 동작 |
| --- | --- |
| Dynamic recompiler | 기본 켜짐. x86-64와 AArch64에서 JIT을 사용합니다. ARMv7에는 JIT이 없습니다. |
| Boot mode | 기본 `auto`. 아래의 SMC 부팅 설명을 참고하세요. |
| LCD persistence | 액정 잔상 효과. 기본 꺼짐. |
| Frame interpolation | 프레임 사이를 섞는 효과. 기본 꺼짐. |
| CPU speed | 기본 100%. 100/125/150/175/200/250/300% 중 선택합니다. |

SMC의 `auto`는 BIOS가 있으면 우선 사용하고, BIOS가 없거나 BIOS 경로의 콘텐츠 로드에
실패하면 직접 부팅을 시도합니다. 다만 `GPMM/` 같은 자작 카드 구조는 기본 BIOS 실행기가
시작하지 못하므로 BIOS가 있어도 카드를 연결한 직접 부팅으로 전환합니다.
`require_bios`는 BIOS를 요구하고 직접 부팅으로 전환하지 않습니다.
`direct_hle`는 BIOS를 사용하지 않습니다. 자작 카드 자동 판별은 이 모드에서도 적용됩니다.

CPU speed는 소리 높이·타이머·화면 갱신 주기를 바꾸지 않고 게임에 주는 CPU 시간을 늘립니다.
느린 게임이나 로딩에 도움이 될 수 있지만 호스트의 CPU 사용량이 늘고 호환성에 영향을 줄 수
있습니다. 필요한 게임에서만 올리세요.

## 저장과 백업

### 게임 안에서 저장

게임 내 저장 위치와 형식은 각 게임이 결정합니다. 압축을 푼 `.smc`를 실행하면
**원본 카드 이미지는 수정하지 않습니다**. 실행 중 카드 읽기·쓰기는 이전과 같게
동작합니다. Windows와 RetroArch는 카드 쓰기가 2초간 멈췄을 때 변경분을 자동으로
기록하며, 기록 시도 사이에는 최소 10초의 간격을 둡니다. 배포판은 디스크 기록을
백그라운드에서 처리하고, 게임을 닫거나 바꿀 때도 마지막 변경분을 저장합니다.
저장 실패 시 기존 파일과 아직 기록하지 못한 변경분을 유지하고 재시도합니다.
Windows는 원본 파일 이름에 `.gp32.sav`를 붙인 파일, 예를 들어 `game.smc` 옆의
`game.smc.gp32.sav`를 사용합니다. RetroArch는 저장 폴더에 `게임이름.gp32.sav`를
사용합니다. 저장 위치에는 쓰기 권한이 필요하고, 변경분은 정확히 같은 원본 `.smc`
위로만 불러옵니다. ZIP은 먼저 압축을 풀어 주세요. RetroArch가 저장 폴더를 제공하지
않으면 시스템 폴더를 사용합니다. 확장자를 뺀 파일 이름이 같은 게임은 저장 경로도
같으므로 서로 다른 저장 폴더를 사용하세요.

예전 전체 카드 저장 파일인 `.gp32.smc`는 자동으로 가져옵니다. 새 페이지 저장에
성공한 뒤 사용된 예전 이미지를 삭제하며, 삭제에 실패해도 새 저장이 유지됩니다.
전체 카드 이미지로 된 `.gp32.sav`도 불러올 수 있습니다. 유효하지 않은 저장 파일이나
원본이 다른 변경분 파일은 덮어쓰지 않고 로드를 중단합니다.

Windows HLE 실행과 RetroArch의 일반 직접 부팅은 쓰기 가능한 카드 장치를 연결하지 않으므로
게임 내 저장을 보존하지 못합니다. RetroArch가 자동 판별한 자작 카드 직접 부팅은 카드를
연결하므로 카드 쓰기를 보존할 수 있습니다. 카드 저장이 없는 경로에서는 스테이트를 사용하세요.

저장 실패 알림이 나오면 최신 진행 상황이 파일에 남지 않았을 수 있습니다. 공간과 쓰기 권한을
확인하세요. 기존 저장 이미지가 읽히지 않으면 이를 보호하기 위해 게임 로드가 중단됩니다.
중요한 진행 상황이 담긴 카드 파일은 별도로 백업해 두세요.

### 세이브 스테이트

Windows에서는 F5/F8 대화상자로 `.gp32st` 파일을 저장·불러옵니다. 기본 제안 이름은
`gp32_state.gp32st`이며, 게임별 자동 파일명이 아니므로 알아보기 쉬운 이름을 지정하세요.
스크린샷도 F12 대화상자에서 저장 위치를 선택하며 기본 제안 이름은 `gp32_screenshot.bmp`입니다.
RetroArch에서는 프런트엔드의 스테이트 저장·불러오기 기능을 사용합니다.

스테이트는 CPU·메모리·연결된 카드까지 포함하는 순간 저장입니다. 카드 변경분은
원본 기준으로 저장될 수 있으므로 해당 원본 ROM을 그대로 보관하세요. 스테이트를
불러오면 게임 내 저장 내용도 그 시점으로 돌아갑니다.

스테이트는 게임 내 저장 이미지와 별개입니다. 다른 빌드나 프런트엔드 사이의 스테이트 호환성은
보장하지 않습니다. 실제 RetroArch 세션에서의 되감기와 런어헤드는 미검증입니다.

## 문제 해결과 제한

- **BIOS 없이 시작 화면에서 멈춤:** BIOS를 지정해 실행하세요. Astonishia Story R, Hany,
  Super Plusha 등은 직접 부팅에 제한이 있습니다.
- **자작 카드가 DATA LOADING에서 멈춤:** RetroArch에서 `Boot mode`가 `auto`인지 확인하세요.
  자작 카드 자동 전환 설명은 RetroArch용이며 Windows 독립 실행에 그대로 적용되지 않습니다.
- **소리가 끊김:** 다른 무거운 프로그램을 닫고, 기기에서 성능 모드를 선택할 수 있으면 확인하세요.
- **어스토니시아 스토리 R 타이틀 음악이 거칠게 들림:** 게임 자체의 음원 디코딩 방식에 따른
  잡음이 남아 있습니다. CPU speed를 높여 해결되는 문제는 아닙니다.
- **Pinball Dreams에서 소리가 없음:** 직접 부팅 경로에서 발생하는 알려진 제한입니다.
- **저장되지 않거나 저장 이미지를 읽지 못함:** 위의 저장 위치, 여유 공간, 권한을 확인하세요.
  기존 저장 파일은 백업한 뒤 상태를 확인하세요.
- **RetroArch가 종료됨:** `retroarch --verbose`로 로그를 확인하세요.

호환성 확인은 일부 장면에 한정되며 전체 게임 완주를 보장하지 않습니다. 실제 GP32와의 소리·입력
지연 비교, 장시간 플레이는 미확인입니다. Android 실행, Qt 실행, 브라우저 재생도 미검증입니다.

프로젝트 출처와 라이선스 고지는 [README](../README.md#credits-and-licensing)와
[licenses/](../licenses/)에 있습니다. Windows ZIP에는 `SDL3-LICENSE.txt`도 들어 있습니다.
