# GP32emu 1.1.0 추가 검토 5차 — 코드 일관성, JIT 매핑, PWM 및 저장 상태

검토·측정일: **2026-10-18 UTC**. 이전 작업에서 이어서 실제 오류를 재현하고, 수정한
전체 코어를 다시 빌드했다. 이번에는 HLE와 CPU가 명령 메모리를 바꾸는 경계, 일반 bus와
JIT의 직접 매핑 계약, PWM의 live counter와 상태 저장을 집중적으로 확인했다.

**새 코드 수정은 통합했고, 네 가지 전체 C23 검사에서 각각 64/64를 통과했다.**
전후 측정에서는 코드 페이지에 함께 있는 데이터의 외부 쓰기 통지와 HLE 문자열 출력을
개선했다. 단일 값 쓰기에 필요한 정확성 검사 비용도 측정했다. x64 조건 판정 최적화
후보 두 개는 인터프리터 대조군이 느려져 기본 코드에 넣지 않았다.

이 문서는 [최초 검토](REVIEW-1.1.0.md), [후속 검토](REVIEW-1.1.0-FOLLOWUP.ko.md),
[심층 검토](REVIEW-1.1.0-DEEP.ko.md), [4차 검토](REVIEW-1.1.0-ROUND4.ko.md)에 이어진다.
이전 원시 측정과 실패 로그는 보존했다. 특히 4차에서 미해결로 남긴 PWM 문제를 이번에
수정했고, 과거 I-cache word-watch 생략 후보는 새 코드와 양립하지 않아 철회 표시했다.

## 1. 결과와 검토 범위

| 영역 | 확인한 문제 또는 기회 | 이번 처리 |
| --- | --- | --- |
| HLE 출력 | 명령으로 번역된 RAM을 바꿔도 CPU에 알리지 않는 출력 경로 | 공통 scalar·문자열·palette·stub·callback·명령 패치 경로 통지 추가 |
| 외부 쓰기 | 같은 4 KiB 페이지의 일반 데이터만 바꿔도 번역 epoch가 바뀜 | 기존 4-byte instruction watch로 실제 겹침을 재확인 |
| 인터프리터·JIT bus | I/O 주소 구간의 유효한 직접 매핑을 캐시형/native 경로가 무시 | 실제 폭·권한의 fastmem을 우선하고 명시적 전체 구간 부재 증명을 도입 |
| 비표준 실행 메모리 | writable BIOS·추가 물리 영역의 명령이 CPU store 이후 남음 | 물리 fetch 추적과 공통/native store 통지 연결 |
| BIOS API | 성공한 firmware 교체 뒤 이전 명령 실행; 실패한 파일 읽기가 기존 BIOS를 덮음 | 성공 시 full flush, 파일 읽기 staging, 겹치는 buffer 입력 처리 |
| 직접 메모리 범위 | 매우 큰 `size_t` 길이에서 덧셈이 wrap되어 잘못 허용됨 | 뺄셈 기반 경계 검사 |
| PWM | buffer/live counter 혼동, stop/manual update/reload 및 타이머 간 DMA 순서 오류 | live 상태 분리, 시간순 이벤트 처리, 반복 IRQ의 안전한 일괄 진행 |
| 저장 상태 | PWM의 미저장 상태와 비정수 속도에서 반복 반올림 | v16/GP32SO10, 92-byte 명시적 trailer, 원래 phase에서 한 번 변환 |
| ARM multiply | raw/cached/native가 같은 실수를 공유할 가능성 | 독립 산술 oracle 872,064개 사례; 제품 수정 필요 없음 |
| x64 predicate 후보 | 대상 native 루프는 빨라지지만 raw 대조군이 느려짐 | 두 후보 모두 보류, 2,128개 측정과 정확한 patch 보존 |
| 오디오 | 큰 resampler 구조체 복사처럼 보이는 코드 | GCC/Clang 생성 코드에서 이미 제거됨을 확인, 불필요한 재작성 보류 |
| WASM | 새 PWM 형식의 32-bit 저장·불러오기와 heap 재사용 | 실제 모듈 두 개에서 반복 검증, 메모리 증가를 할당 위치 변화로 추적 |

집중해서 읽고 수정한 곳은 `src/arm920t.c`, x64/A64 emitter, `src/gp32.c`,
`src/s3c2400.c`와 관련 bus·state 경계다. 기존 오디오 FIR, SmartMedia, codec, loader,
libretro, iOS C bridge, WASM, LCD/IIS/DMA/IRQ 연결은 이전 검토와 통합 회귀 실행에
연결했다. Java/Qt C++ UI나 플랫폼 SDK를 일괄 갱신하지 않았다. Android·iPhone·레트로
기기, 실제 게임 이미지와 펌웨어는 실행 환경에 없었다.

### 기준과 재현 가능한 소스

- 이전 공개 PR head: `2bace9afd9c3d1e5e7cac9d23379e2f60d167fec`.
- 대응하는 로컬 시작점: `85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f`.
- 두 시작점의 동일한 tree: `396aaf687ce5d1ead21db9af44f25185b44ec698`.
- 최종 제품·테스트 동결: 로컬 `865f88e2d095f71ac80935bd775a38f3b8985246`,
  tree `e300f1f9b419f24da5f96b1e831640e47a9a32ce`.
- 이후 커밋은 문서·로그·측정 자료다. 실제 빌드 입력의 SHA-256을 기록해 제품/테스트
  바이트가 유지되는지 확인했다. 최종 공개 커밋과 전달 파일의 해시는 전달 manifest에 기록한다.

로컬 실험 커밋 ID가 모두 공개 Git 객체라는 뜻은 아니다. 외부에서 재현할 때 before는
위 공개 PR head를 사용하고, after는 전달한 최종 소스를 사용한다. 각 로그의 소스 해시와
재구성 patch가 세부 중간 실험을 구분한다. 원래 작업 디렉터리의 수정 파일을 덮어쓰지 않고
별도 worktree에서 진행했다.

전달용 재현 history bundle에는 로컬 동결점과 세 병렬 검토 branch의 Git 객체도 보존한다.
중간 실패·보류 후보를 재현하는 자료이며, 기본 적용 대상은 최종 patch 또는 최종 source다.

## 2. HLE 쓰기와 명령 캐시: 보고되지 않았던 오류를 재현

파일: [gp32.c](../src/gp32.c), [arm920t.c](../src/arm920t.c),
[HLE 회귀 검사](../tests/gp32_hle_memory_test.c),
[외부 쓰기 검사](../tests/arm_external_write_test.c).
증거: [HLE 전후 로그](benchmarks/1.1.0-round5/hle/),
[전후 성능 전체](benchmarks/1.1.0-round5/writes/README.md).

### 출력 값이 맞아도 실행되는 명령은 틀릴 수 있다

`direct_write32_if_ram`과 `direct_write8_if_ram`은 RAM에 올바른 값을 저장했지만,
그 주소가 이미 디코딩되거나 native code로 번역됐다는 사실을 CPU에 알리지 않았다.
문자열·palette·일부 HLE stub 및 callback 설치처럼 이 helper를 이용하거나 직접 RAM을
쓰는 경로에도 같은 문제가 있었다. RAM을 읽으면 새 값인데, 다음 실행에서는 이전
명령이 남을 수 있었다.

이전 EEPROM/zero-fill 수정만으로 전체 HLE 출력이 덮이지 않는다는 것을 확인했다.
공통 scalar helper, bounded cstring, palette mirror와 export, app query, ISR table,
stub/callback 설치, 23개 직접 파일 HLE 명령 패치 지점을 점검했다. 실제 RAM 변경 후
해당 물리 범위를 통지한다. cstring과 일괄 설치에는 범위 통지를 묶었다. halfword의
기존 두 byte 쓰기와 부분 성공 의미, cstring의 512-byte 한도는 유지했다.

GXB relocation 복사는 호출자가 배치 후 전체 flush를 하고 실행을 시작한다는 경로를
확인했다. 이미 필요한 flush가 있는 곳에 같은 통지를 중복 추가하지 않았다.

합성 코드의 결과를 `0x11`에서 `0x22`로 바꾸거나, 조건이 거짓인 명령의 literal 결과를
검사했다. 가능한 곳은 실제 guest SWI를 거쳐 출력시켰다. **14개 writer × 세 실행 경로
× I-cache on/off**와 경계 사례를 포함한 155개 검사에서 수정 전 **60개 assertion**이
실패했다. raw 실행은 통과했고 cached/native에서 stale 명령 결과가 나왔다.
수정 후에는 155개 모두 통과했다. 이는 실제 게임에서 60종 버그가 발생했다는 수가 아니다.

### page보다 더 좁은 범위로 확인하되 기존 정보는 보존

외부 통지는 원래 4 KiB code-page bitmap만 확인했다. 같은 페이지의 일반 데이터만
써도 epoch가 바뀌어서 cached/native block이 다음 진입에서 재검증을 했다. 코어에는 이미
4-byte instruction-word watch bitmap이 있으므로 이를 재사용했다. page가 표시된 경우에만
해당 word 범위와 실제로 겹치는지 확인한다.

첫 bitmap word와 마지막 word의 mask를 따로 제한하고, 32-bit shift 폭을 넘지 않게 했다.
instruction word 일부를 덮는 byte/halfword도 겹침으로 본다. MMU 또는 추적이 불완전한
기존 경로는 계속 보수적으로 무효화한다. 새 RAM word bitmap을 추가하지 않았다.

중요한 전제는 **CP15.I=1에서도 명령을 수집할 때 word watch를 남기는 것**이다.
실제 fetch뿐 아니라 inline leaf를 위한 speculative peek, opcode proof 재검사,
callback-backed fetch도 확인했다. 과거 optional patch인
`skip-unused-word-watches.patch`는 I=1의 기록을 생략하므로 새 정밀 통지와 함께 적용하면
안 된다. 원본 측정은 역사 자료로 남기고 사용 권고를 철회했다.

독립적인 byte-interval oracle로 **12,920개 사례**를 검사했다. bitmap/page/RAM 경계,
부분 명령 덮기, 큰 길이, I on/off, cached/native, MMU alias를 포함한다. 수정 전
**4,356개 불필요한 epoch 변화**를 재현했고 수정 후 제거했다. 이 4,356개는 guest의 잘못된
산술 결과가 아니라 최적화의 불변식 위반이다. 명령을 실제로 덮은 positive case는 계속
무효화되고 새 명령을 실행한다.

## 3. writable BIOS와 추가 실행 영역도 공통 규칙으로 연결

파일: [ARM 코어](../src/arm920t.c), [bus 계약](../src/arm920t.h),
[비표준 영역 회귀 검사](../tests/arm_nonram_code_write_test.c).
증거: [interpreter 상세 기록](benchmarks/1.1.0-round5/interpreter/README.md),
[native 통지 순서](benchmarks/1.1.0-round5/jit/nonram-store-notifier.md).

기존 CPU store 통지는 표준 8 MiB RAM 밖이면 돌아갔다. 일반 bus가 writable BIOS나
추가 실행 메모리를 제공하면 그곳의 번역된 명령을 바꿔도 trace가 남았다. 단순히 그 주소로
처음 실행하면 raw 경로를 택하는 경우가 있어, 피상적인 검사로 놓치기 쉬운 문제였다.

RAM/BIOS caller에서 짧은 `BL` leaf를 수집하는 정상 public dispatch를 사용해 추가 영역
명령이 실제 trace 안에 들어오는 것을 재현했다. 이 경로만으로 **192개 사례 중 56개
assertion**이 수정 전에 실패했다. 디코딩된 내부 구조에 테스트가 임의로 집어넣어야만
성립하는 오류로 결론 내리지 않았다.

새 공통 추적은 BIOS와 다른 비RAM 영역을 별도 flag로 구분한다. 일반 BIOS 실행 때문에
평범한 MMIO/data store까지 매번 무효화되는 것을 피하기 위해서다. CPU의 작은 물리 쓰기와
외부 span 통지가 이 정보를 함께 사용한다. full invalidation은 flag를 비우고, lazy
invalidation은 아직 유효한 watch를 유지한다. 일반 CPU store의 CP15.I=1 cache-maintenance
정책은 기존대로 유지한다.

x64와 A64의 native word-I/O helper도 실제 direct/callback store 직후 공통 notifier를
호출한다. callback 뒤에도 필요한 주소·레지스터 정보를 미리 보관하고, writeback 및
generation/epoch continuation fence보다 앞에서 통지한다. 활성 trace가 자기 명령을
바꿨다면 다음 이전 명령을 계속 실행하기 전에 빠져나오게 된다.

최종 검사는 **10,596개 사례**다. 해당 변경만 제거한 isolated before는
**4,026개 assertion**이 실패하고 after는 모두 통과한다. 그중 3,580개는 보수적 영역
통지의 interval 검사이며, CPU replacement·활성 trace·external replacement·public BL
사례와 단위를 나눠 기록했다. raw는 before에서도 476개 모두 통과한다. 최종 검사에서는
cached 4,564개, native 2,076개 instruction event를 확인했다.

초기 fixture가 cold nonRAM PC의 cached 진입을 잘못 가정해 낸 96개 실패는 별도 보관했다.
예상 명령 결과를 바꿔 통과시킨 것이 아니라 실행 경로를 실제로 성립시키고 public BL
재현을 추가했다. 최종 isolated before를 만드는 reverse patch와 해시도 검증했다.

외부 통지 API는 32-bit 물리 공간 끝에서 잘리는 **nonwrapping 구간**을 받는다. wrap되는
일반 bus 쓰기는 caller가 나눠 알려야 한다. 한편 mapping/권한을 바꾸는 bus는 명시적
전체 mapping flush가 필요하다. 단순한 code-range 통지는 mapping 변경 계약을 대체하지 않는다.

## 4. JIT 직접 매핑: 주소대만으로 callback을 고를 수 없었다

파일: [ARM/bus 코어](../src/arm920t.c), [A64 emitter](../src/arm920t_jit_a64.inc),
[bus header](../src/arm920t.h), [SoC bus](../src/s3c2400.c).
증거: [매핑 증명과 실패 분해](benchmarks/1.1.0-round5/jit/mapping-proof.md).

### raw, cached, native가 같은 메모리를 읽도록 수정

raw interpreter는 실제 물리 주소·폭·읽기/쓰기 권한으로 `fastmem`을 먼저 물었다.
반면 cached의 word/half/byte load/store 여섯 경로와 native 특수 word-I/O 경로는
`0x14000000..0x15ffffff`이면 callback을 먼저 골랐다. generic bus가 그 구간에 직접
메모리를 제공하면 실행기마다 서로 다른 backing store를 읽고 쓸 수 있었다.

표준 RAM/BIOS의 기존 전체 window fast path는 유지했다. 그 밖에서는 정확한 접근 span을
먼저 확인하고, 허용되지 않을 때 기존 callback을 이용한다. 이미 번역한 물리 주소를 쓰므로
MMU 번역을 중복하지 않는다. word rotation, writeback, callback-visible PC와 실행 중단
경계도 함께 검사했다.

서로 다른 두 backing array와 독립적인 permission을 가진 oracle **2,478개 사례**에서
before는 raw 0, cached 932, native 932개 assertion이 실패했다. 최종 조합은 x64,
A64/QEMU, Clang, UBSan에서 모두 통과하며 native 1,576개 instruction을 실제 실행한다.
cached helper만 고친 중간 버전에는 native 특수 경로의 105개 실패가 남는다는 것도 보존했다.

### 한 번의 실패는 전체 구간 부재의 증명이 아니다

첫 native 수정안을 검토하던 중 더 중요한 전제를 찾았다. RAM 끝의 한 byte 접근이
거절됐다는 사실만으로 뒤쪽 전체에 매핑이 없다고 판단할 수 없다. bus는 hole을 가지거나,
byte를 거절하면서 word는 허용하거나, read/write 권한을 다르게 제공할 수 있다.
기존 A64 open-bus 상수 fast path에도 이미 이 추론이 있었다.

이를 `arm_bus_t.fastmem_excludes(user, address, bytes, write)`로 명시했다. nonzero는
**요청한 전체 구간 안의 모든 nonempty subspan에 해당 권한의 직접 매핑이 없다는 약속**이다.
NULL/0은 알 수 없다는 뜻이며, query는 부작용이 없어야 한다. 표준 SoC는 native compilation
때 전체 32 MiB I/O 구간의 read/write 부재를 각각 증명하므로 매 접근의 추가 probe를 피한다.
증명이 없는 generic bus는 정확한 word span을 검사한다.

S3C2400 certificate는 현재 `ram_size`를 읽는다. `get_bus()` 시점의 bool을 복사해 두면
state load로 RAM이 늘어난 뒤에도 CPU가 옛 bus 사본을 사용하는 경우에 틀릴 수 있기
때문이다. 매핑 변경과 full flush 후 compilation이 다시 query하는지도 검사했다.

확장된 native **76개 사례**에서 첫 후보의 잘못된 certificate 추론을 재현했다. 기존 공개
A64 baseline도 I/O 밖의 추가 RAM 주소에서 register 오류 8개가 발생해, 새 테스트 query
횟수만 맞추기 위한 수정이 아님을 확인했다. 최종 x64/A64는 모두 통과한다. 실제 SoC의
복사된 bus를 유지한 채 RAM을 8→16→8 MiB로 바꾸는 상태 복원도 검사했다.

bus field는 끝에 추가해 기존 member offset을 유지한다. 내부 bus 구조체 크기는 바뀌므로
이를 사용하는 코드는 함께 다시 빌드해야 한다. 이 필드 자체를 CPU 저장 파일에 직렬화하지는
않는다. 정상 SoC I/O의 추가 runtime probe가 0이라는 것을 counter로 확인했으며,
물리 AArch64에서 속도가 몇 % 변하는지는 측정하지 않았다.

## 5. BIOS 교체와 범위 검사: 드문 입력도 기존 상태를 지키게 수정

파일: [공개 BIOS API](../src/gp32.c), [SoC loader/fastmem](../src/s3c2400.c).
증거: [BIOS 로그](benchmarks/1.1.0-round5/bios/),
[범위 검사 로그](benchmarks/1.1.0-round5/bounds/).

### 성공한 BIOS 교체는 이전 번역을 폐기

`gp32_load_bios()`와 `gp32_load_bios_data()`는 성공하면 full JIT flush를 호출한다.
같은 BIOS buffer 주소를 그대로 쓰더라도 내용은 바뀔 수 있다. 별도 reset 없이 이 API를
부를 수 있으므로 pointer가 같은 것만으로 이전 명령을 재사용할 수 없다.

CPU의 architectural register·cycle·I 설정은 유지한다. 합성 MOV 명령으로 file/buffer,
raw/cached/native, I=0/1을 검사한 **12개 사례**에서 before는 **8개 stale 실행**이 나오고
after는 모두 통과한다. 실패한 입력은 기존 firmware와 CPU 동작을 유지한다.

### 파일과 overlapping buffer는 먼저 유효한 내용 확보

일반 파일 loader는 기존 BIOS를 먼저 `0xff`로 채운 뒤 읽어서, 부분 `fread` 실패가 나면
기존 BIOS를 망가뜨릴 수 있었다. 크기가 제한된 staging buffer로 읽기를 끝낸 다음 live
BIOS에 반영하도록 바꿨다. allocation/short-read 실패에서는 staging과 file만 정리한다.
ZIP 경로는 이미 staging 방식이었다.

buffer loader에는 현재 BIOS 내부를 source로 주는 alias도 가능하므로 `memmove`를 먼저
하고 남은 tail을 채운다. 먼저 지우면 source를 잃는다. 기존 empty-file erase 동작과
empty-buffer 거절은 서로 다른 기존 API 의미로 유지했다.

별도 **12개 검사**는 실제 partial read 후 실패를 주입하고 512 KiB 전체가 유지되는지,
겹치는 source가 정확히 복사되는지 검사한다. before 2개 실패에서 after 0으로 바뀌었다.

### 범위 검사에서는 덧셈이 먼저 넘치지 않게

`off + bytes > ram_size`, `addr + bytes <= bios_size` 형태는 `SIZE_MAX` 같은 길이에서
먼저 wrap될 수 있었다. 시작점이 범위 안인지 검사한 후 `bytes > size - offset`을
사용한다. read-only BIOS, 끝의 유효한 zero-length span, RAM resize 의미를 유지했다.

확장한 SoC direct-memory 검사 **199개**에서 before 18개 실패를 재현하고 after 모두
통과했다. 일반 CPU의 1/2/4-byte 접근이 이 극단적인 host 길이를 만든다는 주장은 하지
않는다. 공개 core/bus 경계가 자신이 받은 길이를 정확히 검증하도록 고친 것이다.

## 6. PWM: 이전에 남긴 문제를 live counter 모델로 해결

파일: [SoC timer](../src/s3c2400.c), [PWM 독립 모델](../tests/s3c2400_pwm_model_test.c),
[상태 검사](../tests/s3c2400_pwm_state_test.c).
전체 설계·정책: [PWM 보고서](benchmarks/1.1.0-round5/pwm/README.md).

### buffer와 실제 카운터를 구분

기존 코드는 TCNTO를 현재 TCNTB와 elapsed accumulator에서 계산했다. TCNTB를 바꾸면
현재 주기의 count까지 바뀌고, stop 상태에서는 live count 대신 buffer가 보이며,
manual update가 실제 counter load가 되지 않았다. 4차의 미수정 probe에서 이미 재현한
문제다.

[Samsung S3C2400 manual](https://datasheets.chipdb.org/Samsung/S3C2400.pdf) 10장의
double buffering, initialization/stop 설명과 Figure 10-2 파형을 대조했다. 새 모델은
각 timer의 live 16-bit count, terminal 때 포착한 reload, pending reload와 fractional
clock을 분리한다. TCNTB 쓰기는 다음 값만 바꾸고 현재 count를 바꾸지 않는다. manual
update는 실제로 쓴 register lane의 strobe에만 반응하며, 강제 stop은 count와 phase를 보존한다.

양수 N을 manual load한 첫 요청은 N번 감소 후에 생긴다. zero에 도달한 clock에서 요청과
reload capture가 생기고 다음 clock에서 reload한다. zero가 한 clock 동안 보이므로 steady
auto-reload는 N+1 간격이다. terminal DMA가 TCNTB를 바꾸더라도 이미 포착한 다음 reload를
소급해서 바꾸지 않는다. TCMPB는 counter/terminal 간격을 바꾸는 값으로 사용하지 않는다.

### DMA가 관찰하는 다른 타이머도 같은 시각으로 맞춤

timer 0을 한 tick 구간 끝까지 처리한 뒤 timer 1을 처리하면, timer 0의 DMA가 읽는
timer 1 count가 옛 시각일 수 있다. 이제 관찰 가능한 다음 요청 시각까지 다섯 timer를
함께 전진시키고 DMA bus action을 처리한다. 같은 시각의 reload capture를 먼저 끝내며,
그 DMA가 다른 timer를 시작하면 남은 구간만 실행한다.

이미 pending인 IRQ의 반복은 산술로 건너뛴다. `UINT32_MAX` raw tick과 짧은 반복 주기에서
수십억 번 같은 IRQ를 되풀이하는 loop를 피한다. bus 효과가 있는 accepted DMA는 순서를
보존해야 하므로 같은 방식으로 무조건 생략하지 않는다.

이 최적화 중 생긴 오류도 독립 검사로 잡았다. timer 1이 4/10/16 시각에 요청하고 timer 0
DMA가 20에 acknowledge하면 다음 timer 1 요청은 22다. 첫 구현은 생략했던 IRQ를 20에
다시 올렸다. interval의 끝이 실제 terminal인 경우만 요청하도록 고쳐, 같은 검사에서 두
실패가 0으로 바뀌었다. 중간 실패 로그를 남겼다.

최종 독립 모델은 **50개 시나리오, 3,789개 검사, 0 실패**다. before에는 큰 tick stress를
빼고 동일한 49개 시나리오를 실행해 2,504개 assertion 실패를 기록했다. 별도로 기존 4차
probe의 25개 mismatch도 0이 됐다. 이는 timer 모델 검증이며 실제 frame rate 측정은 아니다.

### 여전히 명시적인 emulator 정책인 부분

첫 clock의 fraction 기준, manual update bit를 오래 유지할 때의 효과, zero 초기값,
동시 요청의 tie-break는 실물 계측 없이 확정할 수 없다. 현재 정책은 zero manual/reset을
quiescent로 두고, initial clock은 한 주기 뒤부터 시작하며, 동일 시각 요청은 timer/channel
순으로 결정한다. 테스트는 문서의 manual-update pulse-and-clear 절차를 사용한다.

external TCLK waveform과 TOUT 출력 파형은 이번 구현 범위에 없다. 기존 외부 mux fallback은
유지했다. LCD/IIS/PWM 사이의 전체 SoC tick 순서는 아직 기존 구조다. 이번에 PWM 내부의
시간 순서를 고쳤다는 사실만으로 모든 주변장치 bus arbitration이 cycle-exact가 되지는 않는다.

## 7. 저장 형식 v16: 정보 손실과 두 번 반올림을 피함

파일: [GP32 저장/복원](../src/gp32.c), [SoC 저장/복원](../src/s3c2400.c),
[속도/상태 검사](../tests/gp32_state_speed_test.c).

live count와 이미 포착한 reload는 기존 형식에 없었다. register의 미사용 bit나 C struct
padding에 숨겨 저장하지 않고 명시적 버전을 올렸다.

| 항목 | 형식 |
| --- | --- |
| 전체 machine magic | 16-byte `GP32STATEv0016` header, 끝의 NUL padding 포함 |
| standalone SoC magic | 8-byte `GP32SO10` header |
| 새 trailer | 기존 codec 뒤의 **92-byte little-endian `PWM1`** |
| 공통 값 | 원래 phase clock, pending reload bitmap |
| timer당 값 | live count, captured reload, 64-bit fractional phase |

기존 native-layout SoC body와 RAM·SmartMedia·audio·codec 구간의 byte layout은 유지한다.
새 trailer는 endian을 명시하며 모든 값을 먼저 읽고 검증한 뒤 live 상태를 바꾼다. count,
latch, bitmap, nonzero clock, phase 상한과 pending 관계를 검사한다. phase 변환의 큰
곱셈은 32-bit denominator 기준으로 나눠 중간 uint64 overflow도 막는다.

새 loader는 기존 전체 v2–v12/v14/v15와 standalone SOC6–9/raw 경로를 유지한다.
**v13은 원래도 지원하지 않았고 이번에 새로 제외한 것이 아니다.** 이전 build는 새 magic을
거절하므로 이번에 저장한 v16 파일을 옛 build에서 그대로 읽을 수는 없다.

기존 파일에는 잃어버린 live/latch 정보가 없어서 무손실 복원이 불가능하다. 옛 observable
count를 표현 가능한 범위에서 보존하고 나머지는 정해진 migration 정책을 쓴다. 실제 이전
baseline writer로 만든 848,482-byte SOC9 fixture와 SHA를 보존했다. 그 구형 body는
native LP64/little-endian 자료이며, 새 trailer가 portable하다는 것과 전체 구형 형식이 모든
ABI 사이에서 portable하다는 주장은 구분한다.

133% 실행 속도에서 저장할 때 먼저 100%로 내리고 다시 133%로 돌리면 두 번 floor되어
phase가 달라질 수 있었다. 새 trailer는 저장 전 원래 phase와 clock을 사용한다. load도
최종 host speed를 정한 뒤 source phase에서 target으로 한 번만 변환한다. 동일 133%에서
400-cycle probe에 나온 **15개 count divergence**가 0이 됐다.

PWM state **105개 시나리오**는 92개 trailer truncation, malformed/range/maximum 입력,
legacy load와 clock 변환을 검사한다. whole-machine speed state **62개 사례**는
100/133/150%, file/buffer, 저장·load 직후의 92-byte trailer 비교, 이후 400회의
single-cycle count·IRQ 비교와 v15 decode를 포함한다. 모두 통과했다. 기존 일부 fixture는
TCNTB만 쓰고 manual load를 생략했으므로
문서에 맞는 초기화 pulse를 추가했다. 원래 기대했던 architectural 결과를 삭제하지 않았다.

옛 전체 상태의 다른 주변장치 normalization까지 모두 재설계하지는 않았다. 탐색적 wire
동일성 probe의 17개 차이 중 이번 PWM과 연결된 15개를 없앴고, 기존의 넓은 peripheral
round-trip 비교에서 남은 두 차이는 기록했다. 임의 속도에서 모든 과거 state byte가
완전히 round-trip된다는 결론은 내리지 않는다.

## 8. 성능: 좋아진 조건과 실제 추가 비용을 함께 기록

원시 표본·모든 조건: [외부 쓰기/HLE 결과](benchmarks/1.1.0-round5/writes/README.md).
측정 driver는 별도 검토를 거쳤으며
[그 검토 범위와 해시](benchmarks/1.1.0-round5/jit/benchmark-driver-review.md)도 보존했다.

AMD EPYC 9V74 x86-64, CPU 0 affinity, GCC 13.3/Clang 18.1.3, 같은 최적화 flag,
profiling off로 비교했다. 각 조건마다 AB/BA 순서를 바꾼 7쌍을 실행했다. 외부 쓰기
560개와 HLE 448개, **총 1,008개 timed sample**을 수집했으며 outlier를 제거하지 않았다.
별도의 profile-on/clock-off 실행으로 native 경로와 동일한 guest work를 확인했다.

| 작업 | GCC / Clang 전체 조건에서 관찰한 변화 | 해석 |
| --- | --- | --- |
| 같은 code page의 data 외부 통지 + cached 실행 | 약 27–34% 시간 감소 | false-positive epoch와 반복 재검증 감소 |
| 같은 작업 + native 실행 | 약 49–54% 시간 감소 | 두 명령으로 된 hot component에서의 결과 |
| 다른 page의 data 통지 | 약 1.1–2.2 ns 추가 | 새 범위/추적 검사 비용; 약 7.6–17.4% |
| 실제 watched word와 겹치는 통지 | 약 2.3–4.7 ns 추가 | 필요 invalidation은 유지; 약 6.2–16.0% |
| HLE word/byte 출력 | 약 2.2–3.7 ns 추가 | 누락된 code notification을 복원한 비용 |
| HLE 32-byte cstring | 약 82–84% 시간 감소 | byte별 gateway를 bounded bulk copy로 묶음 |
| HLE 256-byte cstring | 약 97–98% 시간 감소 | 이 길이의 56개 pair가 모두 개선; 두 길이를 합치면 112개 |

외부 통지의 ns/call에는 host data store, optional notify, 두 번의 register 설정,
`MOV; BX` 두 명령 dispatch가 들어 있다. notifier 함수 하나만의 latency가 아니다.
HLE는 timing 안에서 guest instruction을 전혀 실행하지 않고 writer gateway만 측정한다.
각 sample 뒤에는 시간 측정 밖에서 전체 8 MiB RAM, PC/CPSR/register와 work를 확인한다.

HLE scalar의 이전 gateway는 약 2.5–3.1 ns였으므로 2–4 ns 증가는 상대값으로
약 71–136%다. 이런 비용을 숨기고 문자열의 큰 감소율만 제시하지 않는다. 올바른 code
notification은 필요한 수정이다. 대량 출력은 묶고, 일반 data 쓰기에는 word refinement를
사용해 그 정확성 비용을 줄이는 방향을 채택했다.

이번 비교는 **4차 전체 소스와 5차 통합 소스의 비교**다. word refinement 외에도 mapping,
nonRAM 추적, bounds 수정 및 컴파일러 code layout 영향이 포함된다. no-notification
대조군도 -3.34~+3.28% 움직였다. 모든 차이를 특정 C 함수 하나의 효과로 돌리지 않는다.
실제 game별 해당 호출 빈도, CPU/GPU 병목, frame pacing이 없으므로 FPS·배터리·AArch64
기기 개선율로 환산하거나 이전 최적화의 %와 더하지 않는다.

## 9. JIT에서 시도했지만 채택하지 않은 두 최적화

상세 조건 38개씩의 전체 결과: [predicate 실험](benchmarks/1.1.0-round5/jit/predicate-results.md).
최종 `x64_emit_cond_skip`는 baseline 구현을 유지한다.

첫 후보는 HI/LS를 masked C/Z 비교로, GE/LT/GT/LE를 shift/XOR/test로 줄였다. 두 번째는
HI/LS만 남긴 작은 후보였다. operand forwarding에 사용하는 EAX를 보존하고 helper 뒤의
flags·join·retirement 규칙을 유지하도록 했다. 모든 16 condition × 16 NZCV와 callback
변경을 별도 oracle로 확인했으며, 해당 256-case 검사에서 native 1,792개 명령을 셌다.

| 후보 | native 쪽 관찰 | 함께 발견한 비용 | 판단 |
| --- | --- | --- | --- |
| compound predicate 전반 | 대상 GCC −7.72~+0.11%, Clang −7.04~−2.17%; HI/LS block 702→662 byte | GCC raw 대조군 약 5.3–7.9% 증가, 일부 cached/cold 결과 혼재 | 보류 |
| HI/LS만 변경 | GCC 약 1.2–4.5%, Clang 약 2.0–4.5% 감소 | GCC raw 대조군 약 7.7–9.1% 증가, paired median도 약 8.0–9.9% 증가 | 보류 |

변경한 emitter를 실행하지 않는 raw 대조군도 느려졌다. 첫 후보에서는 GCC
`exec_arm_at` register allocation/크기가 달랐다. 작은 후보는 이 함수의 주소·크기·정규화한
assembly까지 같았는데도 저하가 남았다. 주변 배치·실행 비용의 영향 가능성은 있지만,
원인을 확정했다고 쓰지는 않는다. GCC만 예외로 두는 기본 gate도 추가하지 않았다.

첫 runner의 case 회전은 의도와 다르게 각 case 안의 before/after 순서를 고정시켰다.
그 실험은 case 전체로는 두 순서가 있지만 개별 case의 교대 측정이라는 설명이 틀렸다.
원시 결과와 원래 runner를 보존하고 문서를 정정했다. 작은 후보의 후속 실험에서는 stable
case index로 각 case의 AB/BA 교대를 검증했다. 두 연구의 **2,128개 timed sample**을
그대로 남겼다. 이번 외부/HLE 연구와 합치면 새 timed sample은 **3,136개**다.

이런 결과는 작은 generated code가 항상 전체 에뮬레이터를 빠르게 만든다는 가정을
경계하게 한다. 사용자가 이전에 게임·기기마다 달라 채택하지 않은 최적화와 같은 종류의
문제다. 이번에도 실제 손실이 있는 후보를 기본 코드에 억지로 넣지 않았다.

## 10. ARM multiply, 오디오, 기존 실험의 재검토

### 지원되는 multiply는 독립 산술로 확인

[새 multiply 검사](../tests/arm_multiply_test.c)는 MUL/MLA/UMULL/UMLAL/SMULL/SMLAL의
경계값, sign/zero extension, overflow, accumulator, 유효한 register alias, flag 동작을
독립 C 산술로 계산한다. 세 실행기가 서로 같은 오류를 갖고 있어도 비교가 통과하는 문제를
피하기 위한 검사다.

**872,064개 사례**가 GCC, Clang, A64/QEMU, UBSan에서 통과했다. 각 실행 경로당
290,688개이며 native instruction 581,376개와 multiply semantic helper fallback 0을
확인했다. 초기 A64 coverage 검사는 모든 semantic helper가 0일 것이라고 잘못 가정했다.
실제 호출은 BX return helper였으므로 multiply-kind helper가 0인지 확인하도록 검사를
수정하고 다른 helper 수는 별도 출력했다. 산술 기대값은 바꾸지 않았다. 새 multiply
product optimization이나 오류 수정은 이 결과로 정당화되지 않았다.

### 큰 C 구조체 복사처럼 보여도 실제 비용은 이미 없어질 수 있음

`gp32_audio_resampler_output_frames()`는 약 33.6 KiB resampler를 복사하는 형태여서
호출당 큰 복사가 의심됐다. 실제 GCC13/Clang18 `-O3` object에서는 필요한 rate/phase/
have-prev scalar만 읽고 stack copy나 `memcpy`가 없었다. libretro에서도 정상 매 audio
block마다 이 query를 부르는 것이 아니라 queue가 한계에 가까울 때 쓰는 경로였다.
[assembly와 기록](benchmarks/1.1.0-round5/audio/)을 남기고 no-op 최적화는 하지 않았다.

기존 A64 FIR도 4차에서 history accumulation의 자동 vectorization을 이미 확인했다.
모든 경로가 scalar라고 가정해 수동 NEON을 새 성과로 만들지 않았다. packed TLB,
paired operand load, leaf/frame tracing, RAM mapping reuse와 optional A64 두 번째 way
chaining은 과거 시도와 결과를 읽고 중복 실험을 피했다.

### 질문했던 DSP 명령과 오디오 하드웨어

`SMLAxy`, `SMLAWy`, `SMULWy`, `SMLALxy`, `SMULxy`는 ARMv5TE 계열이고 ARM920T의
ARMv4T 명령이 아니다. 일반 `SMULL`/`SMLAL`은 별개로 지원된다.
[ARM920T TRM](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb)과
[ARM ARM](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a)을 기준으로
이전 검토에서 확장 control encoding의 undefined 경로를 고쳤으며 이번 multiply 검사는
지원 명령의 계산을 확인한 것이다.

별도 audio DAC의 유무가 ARM CPU의 ISA를 늘리지는 않는다. S3C2400에는 IIS와 DMA가
있고, 외부 audio 부품과의 연결 자료는 [이전 하드웨어 조사](REVIEW-1.1.0-FOLLOWUP.ko.md#2-별도-오디오-칩과-회로-자료)에 정리했다.
이번에 새 완전한 GP32 board schematic이나 사용자의 게임 opcode 실행 빈도를 확보하지는
않았다. 게임 image 안의 bit pattern 존재와 실제 실행되는 instruction은 구분해야 한다.

## 11. C23 및 통합 검증

상세 빌드 flag·source hash·raw log: [최종 검증](benchmarks/1.1.0-round5/verification/README.md).

| 실행 환경 | 최종 결과 | 확인한 범위 |
| --- | --- | --- |
| GCC 13.3 x86-64 Release | **64/64** | 새 build directory, 전체 inventory |
| Clang 18.1.3 x86-64 Release | **64/64** | 별도 fresh build |
| GCC 13.3 AArch64 + QEMU 8.2.2 | **64/64** | A64 생성 코드 포함, 기능 검사 |
| GCC 13.3 + UBSan | **64/64** | 전체 compile/link sanitizer, recovery off, runtime diagnostic 없음 |
| GCC ASan 집중 검사 | **5/5** | fastmem, BIOS staging/reload, PWM state, speed state; 한 번씩 순차 실행 |
| Clang/LLD18 WASM + Node24 | 통과 | 기존 PCM/clock/VFS/save gate, production/test 두 module의 반복 load/save |

네 전체 CMake 구성은 C23 required, CPU profile on, IPO off, headless/libretro/tests on이다.
각각 실제 **82개 compile command**와 **194개 입력 해시**를 보존했다. GCC는
`-std=c2x`, Clang은 `-std=c23`을 사용한다. GCC13의 `__STDC_VERSION__`은 여전히
`202000L`이다. 지원 툴체인의 C23 dialect 빌드 성공을 C23의 모든 library feature 지원과
동일시하지 않았다. 단지 최신 문법을 쓰기 위해 `<stdbit.h>`를 새로운 필수 의존성으로
도입하거나 호환 compiler를 제거하지 않았다.

빌드에는 기존 warning과 shared fixture의 warning이 남아 있다. 모든 경고 위치·type·caller를
기준 baseline과 대조한 검토에서 새 production correctness 문제는 찾지 못했다. raw
diagnostic 수는 white-box core 중복 compilation 때문에 많으며, warning-free라고 쓰지 않는다.
ASan은 `detect_leaks=0:halt_on_error=1`이므로 leak 검사까지 통과했다는 의미가 아니다.
WASM에는 native 전용 새 `fastmem_excludes` helper의 미사용 warning도 남는다. portable
backend에서 이 compilation-time helper를 호출하지 않는다는 뜻이며, 경고를 숨기지 않았다.

### WASM은 실제 반복 load/save까지 확인

[WASM 기록](benchmarks/1.1.0-round5/wasm/README.md)의 기존 gate는 32번 저장을 검사했지만
load는 호출하지 않았다. 기존 gate를 그대로 통과시킨 뒤 보조 driver로 실제 public load/save를
추가했다. production/test module 각각 32회, 다섯 timer의 160개 서로 다른 count/phase 조합을
넣어 92-byte trailer 전체, CPU PC/cycles, VFS 정리를 확인했다. BIOS를 guest로 실행하지는
않으므로 이 검사는 state codec/ownership 경계의 검사다.

새 state는 **9,290,654 byte**로 이전보다 정확히 92 byte 크다. save-only heap은
38,340,768 byte로 유지됐다. load/save에서는 heap top이 38,340,768과 46,729,392 byte를
번갈아 나타냈다. allocator header chain을 따라가니 live allocation은 10개로 일정하고,
기존 save staging 주변에서 replacement 8 MiB RAM의 위치가 번갈아 바뀌었다.

따라서 이 반복에서 누적 leak은 없었다. 두 번째 load의 +8 MiB를 보고 고정 heap assertion을
성급히 production bug로 바꾸지 않았다. 전체 할당 궤적과 초기 harness 오류를 보존했다.
이는 임의 게임/state에 대한 보편적 메모리 상한은 아니며, WASM linear memory의 기존
initial 128 MiB 설정과 heap 사용량도 같은 수치가 아니다.

## 12. 다음 최적화의 우선순위와 필요한 증거

이번에 채택한 코드는 파일과 회귀 검사로 제공한다. 다음 후보는 아직 기본 코드에 넣지
않았으며, 실제 profile 또는 별도의 설계 검증이 있어야 판단할 수 있다.

| 우선 검토 | 왜 필요한가 | 채택 전에 확인할 것 |
| --- | --- | --- |
| 낮은 사양 기기에서 실행 경로별 빈도 | scalar 통지 비용과 code-page 재검증 절감 중 무엇이 큰지 게임마다 다름 | raw ARM/Thumb/cached/native 비중, helper·epoch·cold compile 빈도, frame time 분포 |
| invalidation의 dependency 범위 축소 | 현재 필요한 한 word 변경도 generation/epoch 수준에서 더 넓게 영향을 줄 수 있음 | cross-page trace, inline leaf, MMU alias, callback 중 변경, mapping flush를 포함한 독립 coherence oracle |
| predicate emitter의 배치 영향 분리 | 작아진 native block 이득보다 raw 대조군 손실이 남음 | translation-unit 분리 등 한 가지 가설로 raw/cached/native/cold를 모두 비교; source assembly 동일성만으로 결론 내리지 않기 |
| 기존 optional A64 두 번째 way chaining | 첫 way miss가 많은 기기/게임이면 dispatcher 비용 절감 가능 | 실물 A64의 hit/miss 분포, code size, 첫 compile·arena 비용 및 기존 regression 유지 |
| Thumb decoded/native 경로 | Thumb 비중이 높으면 현재 raw 수행량이 병목일 수 있음 | 먼저 사용 비중 확인; branch/PC/flags/bus callback/IRQ/stop 경계에 별도 설계와 oracle 필요 |
| LCD/IIS/PWM의 공통 시간 순서 | DMA가 다른 주변장치를 관찰하거나 바꿀 때 장치 사이 순서가 중요 | 공통 deadline 설계와 bus action 시각 oracle, 단순 tick 일괄 처리로 관찰 가능한 요청을 건너뛰지 않기 |
| state staging의 최대 메모리 사용 | WASM에서 load/save 중 추가 RAM 배치가 관찰됨 | failure atomicity·ownership 유지, 크기 변경/손상 파일/연속 저장 복원의 peak 측정 |

SIMD, branchless C, 더 큰 code cache, 더 적은 검사를 일괄 적용하는 방식은 선택하지 않았다.
현재 근거로는 **잘못된 실행을 만드는 경계를 고치고, 불필요한 재검증과 반복 gateway를
줄이는 변경**이 명확한 성과다. HLE scalar와 true-code 통지의 추가 비용, 새 state 형식의
역방향 호환 제한, 실물에서 아직 확인하지 못한 PWM 정책도 함께 검토해야 한다.

## 자료 찾아보기

- [5차 전체 증거 색인](benchmarks/1.1.0-round5/README.md)
- [채택한 외부 쓰기/HLE 성능 전체 표](benchmarks/1.1.0-round5/writes/README.md)
- [보류한 x64 JIT predicate 비교](benchmarks/1.1.0-round5/jit/predicate-results.md)
- [interpreter/multiply/nonRAM 독립 검사](benchmarks/1.1.0-round5/interpreter/README.md)
- [PWM 모델·호환성·모든 정책](benchmarks/1.1.0-round5/pwm/README.md)
- [최종 C23 검사와 독립 통합 검토](benchmarks/1.1.0-round5/verification/README.md)
- [ASan](benchmarks/1.1.0-round5/asan/README.md), [WASM](benchmarks/1.1.0-round5/wasm/README.md)

Draft PR은 [#1](https://github.com/gajae1/gp32emu/pull/1)에서 이어서 검토한다.
이번 작업은 1.1.0 release 또는 main merge를 수행하지 않는다.
