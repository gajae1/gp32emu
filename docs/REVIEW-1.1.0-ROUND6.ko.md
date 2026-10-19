# GP32emu 1.1.0 추가 코드 검토 — Round 6

## 기준과 범위

이 작업은 PR #1의 `06cf63f523f360c583120bcff62064cb2b53d6ed`에서 시작했다.
이전 `gp32emu-pr1-round441-validation.zip`의 실제 기기·게임 결과와
`docs/REVIEW-1.1.0-ROUND5.ko.md`를 복구한 뒤, 중단된 코드 검토를 이어 갔다.
이 문서에서 새로 수행한 검사는 합성 입력을 이용한 소스·실행 검사다.
실제 BIOS/게임, Windows/Mac 프런트엔드, H700 기기 검증은 별도 후속 작업이다.

검토 우선순위는 명령 캐시 무효화, LCD/IIS/PWM의 관찰 가능한 이벤트 순서,
저장 상태의 소유권·실패 보존·호환성이다. 기존 MMU/별칭/자기 수정 코드 검사와
정확한 PWM 상태 복원은 유지하며, 효과가 없었던 PWM 나눗셈 최적화는 적용하지 않는다.

## 수정 설계와 재현 근거

### 1. HLE PCM 채널과 소수 재생 위치

기존 저장 형식은 첫 번째 활성 PCM 채널의 레거시 미러만 기록했다. 성공한
저장·복원 뒤에도 두 채널 중 하나가 사라졌고, 11025 Hz 소스의 소수 진행 상태가
0으로 바뀌었다. 채널 1만 남은 경우에는 채널 0으로 이동했다.

새 전체 상태 magic은 `GP32STATEv0017`이다. CPU 번역 상태 다음, SoC 상태 전에
164-byte `PCM1` 구간을 추가한다. 네 채널 각각의 활성 여부, RAM 주소, 크기,
바이트 위치, 반복, 소스 주파수, 스테레오, 비트 수와 64-bit 소수 누적값을
명시적인 little-endian으로 기록한다. 기존 native 구조체 구간은 바꾸지 않는다.

모든 채널은 들어오는 RAM 크기를 기준으로 검증·준비한 뒤 SoC commit 후 한 번에
설치한다. 이 단계에는 추가 할당이나 실패할 수 있는 처리가 없다. 비활성 채널은
완전히 비워져 있어야 하며, 활성 채널의 위치·주파수·형식·분수 범위를 확인한다.

v2–v12/v14/v15/v16 읽기는 유지한다. 이전부터 제외된 v13 처리는 바꾸지 않는다.
옛 상태에 기록되지 않은 채널·분수 정보는 복원할 수 없으므로 기존 마이그레이션을
유지한다. 옛 코어는 새 v17 상태를 읽지 못한다. 새 상태는 같은 v16 상태보다
정확히 164 bytes 커진다. SoC `GP32SO10`과 끝의 92-byte PWM 구간은 유지한다.

### 2. libretro 복합 상태의 실패 보존

기존 `retro_unserialize`는 게스트를 먼저 복원하고 LRST 구간을 읽었다.
손상·잘림·오디오 큐 할당 실패로 false를 반환해도 게스트와 출력 이력이 바뀌었다.
LRST magic만 남은 짧은 헤더를 구간 없음으로 잘못 받아들이는 경우도 재현했다.

새 `gp32_load_state_data_checked` API의 선택적 tail validator는 전체 게스트
입력이 검증된 뒤, 첫 live commit 전에 호출된다. libretro는 이 시점에 LRST를
검증하고 필요한 큐 메모리를 별도로 확보한다. 이후 적용은 실패할 수 없는 복사다.
거절 시 준비한 메모리만 해제하고 기존 게스트·오디오·영상 이력을 보존한다.
기존 API와 파일 읽기는 callback 없이 동일하게 동작하며, LRST v2 형식은 유지한다.

### 3. SmartMedia 상태 검증과 32-bit 계산

손상된 블록 shift 또는 주소 cycle 수는 이후 NAND 명령에서 32-bit 범위 밖
shift를 유발했다. raw `bool` 값 2도 실제 UBSan 오류를 일으켰다. 이러한 표현은
live 상태에 접근하기 전에 거절한다. `bool`은 값을 평가하기 전에 표현 바이트를
유효한 false/true 표현과 비교한다.

페이지 수 × 페이지 크기는 64-bit에서 비교한다. erase는 페이지 범위를 먼저
확인하고, 블록 바이트 수를 64-bit에서 남은 카드 범위로 제한한 뒤 `size_t`로
변환한다. wasm32에서 범위 밖 페이지 `0x7c1f20`의 계산이 byte 12800으로 감겨
작은 카드 끝부분을 지우던 경로와, 유효 범위의 shift 29가 블록 길이를 0으로
감기게 하던 경로를 합성 카드로 검사한다.

### 4. LCD/IIS/PWM과 CPU의 이벤트 순서

기존에는 요청 구간 전체의 LCD, IIS, PWM을 차례대로 처리했다. IIS DMA가
PWM count나 LCD 상태를 읽거나 MMIO를 바꾸면 큰 tick과 1-cycle 진행이 달랐다.
DMA가 실행 중 명령을 바꾸거나 IRQ를 unmask하는 경우 CPU도 이벤트를 지나쳐
실행할 수 있었다.

조용한 구간을 먼저 처리하고 다음 관찰 가능한 이벤트의 1 cycle을 처리한 뒤
deadline을 다시 계산한다. 같은 cycle의 기존 LCD → IIS → PWM 순서는 유지한다.
MMIO/범위 밖 RAM/현재 및 reload 소스 범위를 증명할 수 없는 IIS DMA는 다음
프레임까지 CPU와 장치의 실행을 제한한다. 안전한 RAM → 고정 IIS FIFO 전송과
이미 pending인 반복 PWM은 기존 대량 처리를 유지한다.

LCD를 관찰하거나 바꿀 수 있는 DMA가 있을 때 필요한 프레임 경계도 지킨다.
그러한 관찰자가 없는 경우 기존의 여러 LCD 프레임을 한 번 렌더링하는 정책은
유지한다. 따라서 이 수정은 모든 tick 크기의 전체 내부 상태 바이트가 항상 같다는
주장이 아니다. 비교 기준은 에뮬레이터의 기존 1-cycle 이벤트 정책이며 실리콘의
동일 cycle 우선순위를 새로 확정한 것이 아니다.

### 5. 번역 코드 watch 비트맵 — 측정 후 최적화 제외

검증된 연속 명령 범위를 명령마다 OR하던 루프를 비트맵 워드 범위 갱신으로
바꾸는 후보를 구현했다. 범위·오버플로·MMU·비RAM 판단을 유지했고 전체 bitmap
oracle도 일치했다. 그러나 고정된 작업량의 전후 측정에서 큰 블록의 이득과 함께
일반 실행 경로의 회귀를 확인했으므로 최종 패치에서는 원래 루프로 되돌렸다.

최종 `src/arm920t.c` 전체는 기준 06cf63f와 byte-for-byte 같으며 SHA-256은
`8e390d734ecd6195a40044341ed841af092492104c3267e53831907b3bd7440c`다.
확대한 외부 쓰기 구간 oracle과 후보·고의 오류 mutant·원시 측정 자료는 남겼다.
보수적인 MMU 무효화나 기존의 별칭·번역·active SMC 보호를 줄이지 않는다.

### 6. DMA가 진행 중 자신을 정지하는 경우

DMA가 자기 DMASKTRIG에 STOP을 쓰면 register handler는 채널을 정지하지만,
호출 중인 전송 루프는 이전 count와 주소를 되돌려 쓰거나 다음 MMIO 전송까지
계속했다. 중첩 DMA가 STOP 뒤 다시 ON을 쓰면 ON 비트만 확인해서도 구별할 수 없다.

채널별 64-bit 취소 세대 값을 추가해 현재 bus unit을 마친 뒤 STOP 발생 여부를
확인한다. 완료한 RAM 쓰기의 명령 캐시 무효화는 유지하고, 취소된 전송의 주소·
count·reload·종료 IRQ는 반영하지 않는다. 이 값은 실행 중 비교에만 쓰며 저장
형식에는 넣지 않는다. 기존 4,160 copy oracle과 72 trigger 검사에 STOP·중첩
STOP/ON 68개 시나리오를 추가했다. 기준 코드는 64개가 실패했고 수정 후 모두 통과했다.

STOP의 기준은 [Samsung S3C2400 설명서](https://datasheets.chipdb.org/Samsung/S3C2400.pdf)의
DMA 8-13쪽에 있는 현재 atomic transfer 완료 후 정지·현재 count/주소 초기화
규칙이다. 같은 설명서 8-9/8-10쪽은 TSZ[27]의 burst 길이를 네 단위로 정의한다.
이번 수정은 기존 구현의 단위 전송에서 STOP 결과를 보존하며, 아직 모델링하지
않은 burst의 원자 경계까지 검증한 것은 아니다.

### 7. WASM과 웹 상태 복원의 오류·소유권

상태 작업 실패는 기존 게스트가 보존돼도 WASM 상태를 ERROR로 바꾸어 실행·저장·
재시도를 막았다. 상태 작업의 오류 메시지는 제공하면서 READY/RUNNING은 유지한다.
실행 전 load처럼 일찍 거절하는 경로도 호출자가 넘긴 입력 버퍼를 해제한다.
웹에서는 복원을 거절하기 전에 비우던 오디오 큐를 성공한 commit 뒤에만 비운다.

웹 헤더 검사는 v2만 허용하던 목록을 코어의 v2–v12/v14–v17과 일치시켰다.
WASM 할당 실패는 주소 0에 쓰기 전에 검사하고, 상태 입력 준비 실패는 실행을
일시 중지하기 전에 처리한다. 공통 복사 helper를 쓰는 BIOS·게임 로더도 오류를
표시한다. 게임 이름 할당이 실패하면 먼저 확보한 게임 버퍼를 해제한다. 이를 위해
아직 코어로 넘기지 않은 버퍼를 반환할 `gp32_wasm_free`를 export한다. 빈 BIOS·
게임 파일은 할당 전에 거절하며, 성공한 C 호출 뒤에는 소유권이 이미 넘어갔으므로
웹에서 다시 해제하지 않는다. 웹 JS와 WASM 바이너리는 함께 재빌드해야 한다.

## 최종 기능 검증

최종 구현 고정 커밋은 `41a86b7998574515f107f5a553597d3e73e8eced`이다.
첫 구현 checkpoint `e1e85286734ad4179621e48510226a8e922728fa`의 검사 뒤
DMA STOP과 WASM/web 문제를 추가로 고쳐 `83fe33a8`로 고정했다. 그 버전의
기능 검사는 통과했지만, 뒤의 성능 검사에서 watch 최적화를 제외하기로 결정했다.
아래 검사는 제외를 반영한 최종 41a86b7 소스를 새 빌드 디렉터리에서 다시 빌드한
결과다. 기능 검사와 성능 측정은 분리했다.

| 환경 | 설정 | 최종 결과 |
|---|---|---|
| GCC 13.3 / Linux x86-64 | C23, Release, profile ON, IPO OFF | 64/64 통과 |
| Clang 18.1.3 / Linux x86-64 | C23, Release, profile ON, IPO OFF | 64/64 통과 |
| GCC 13.3 + UBSan | C23, RelWithDebInfo, undefined 오류 즉시 중단 | 64/64 통과 |
| AArch64 GCC 13.3 / QEMU 8.2.2 | C23, Release, native A64 JIT 포함 | 64/64 통과 |
| clang/wasm-ld 18 + Node 24.19 | 실제 wasm32 freestanding 런타임 | build/test 통과 |
| 실제 웹 함수의 Node VM 검사 | 버전·거절·OOM·입력 소유권·재개 | 97 checks, 0 failures |

이전 후보 83fe33a8의 UBSan 최초 전체 실행과 첫 재실행은 `s3c2400_irq_test` 하나를 실행하지 못해
63/64였다. 해당 파일이 0644였고, 실행 권한 복구와 전체 CTest를 같은 명령 안에서
수행하자 64/64가 통과했다. 복구 전후 바이너리 SHA-256은
`bf077b8ac5329b6615943c7ef24d61e7a7885c498536ab847a085bad99a57950`으로 같다.
이전 checkpoint의 AArch64에서도 다른 파일에 같은 종류의 권한 문제가 있었다.
최종 41a86b7의 빌드 디렉터리는 `/tmp` 아래에 두었고 네 환경 모두 최초 전체
실행에서 64/64였다. 권한이 되돌아간 원인 자체는
확정하지 않았으며, 실패 로그·권한·동일 바이너리 해시와 최종 재실행 로그를 함께
보존한다. 코드나 sanitizer 설정을 바꿔 통과시킨 결과가 아니다.

각 native 검사는 시작 커밋·tree·컴파일 명령·전체 소스 SHA-256·test inventory·
실제 로그를 기록했고, 검사 후 소스가 고정 버전과 같은지도 확인했다. WASM에는
웹 JS를 포함한 별도 소스·최종 production/test module 해시를 남겼다.

| 추가 oracle / 재현 | 기준 코드 | 수정 후 |
|---|---|---|
| PCM 연속 재생 | 다중 채널 손실, 분수 초기화, 채널 이동 재현 | buffer/file 10 시나리오, 164 잘림, 52 손상 필드 검사 통과 |
| libretro 복합 상태 | 7개 거절 시나리오에서 8개 보존/거절 assertion 실패 | 잘림·손상·오디오 큐 OOM에 게스트와 delivery 보존 |
| SmartMedia | 범위 밖 shift 3종·raw bool UBSan 오류, wasm32 14 assertion 실패 | native/UBSan/wasm32 거절·erase 범위 검사 통과 |
| 장치 이벤트 | 15 시나리오 × 3 모드 × 4 tick 크기 중 72 불일치 | 180 비교 모두 일치 |
| 독립 장치 differential | 합성 설정 500개 | 첫 scheduler 고정본 e1e8528에서 통과; 후속 STOP 추가 전 검사 |
| DMA STOP | 새 68 시나리오 중 64 실패 | 68/68 통과; 기존 copy 4,160 및 trigger 72 유지 |
| watch 비트맵 | 전체 비트맵 비교 기준 | 46,080 span 완전 일치 |
| 외부 쓰기 구간 | 기준과 후보에 같은 oracle 적용 | 각각 342,896 구간 통과; 고의 누락 mutant는 21,256 실패 |
| WASM 상태 작업 | 거절 후 ERROR로 실행·재시도 차단 | READY/RUNNING·게스트·입력 소유권 보존 |

독립 리뷰의 최종 검토 파일 8개가 최종 커밋과 일치함을 확인했다. 리뷰에서 발견한
테스트 erase 주소 정렬, WASM 증분 빌드 의존성, VFS 실패 fixture의 준비 방식,
공통 copy helper의 두 번째 할당 실패 시 소유권 문제는 모두 보완했다.
실기 DMA 우선순위·브라우저 오디오 장치·실제 게임 성능은 이 승인 범위에 포함하지 않는다.

## 성능 판단과 제외한 후보

### watch 최적화의 전후 대조

다른 빌드·테스트를 멈춘 구간에서 GCC/Clang, portable/native, MMU off/on,
2/8/32/96 명령, warm dispatch/변경 없는 코드 재검증/변경된 코드 재번역의
96개 조합을 각각 7쌍 AB/BA 교대로 측정했다. 총 1,344개 표본을 모두 보존했다.
profiling은 OFF이며, 별도 ON 실행으로 native 명령 실행을 확인했다. 레지스터,
CPSR, cycles, epoch/generation, 전체 RAM 등의 동일 작업량 oracle은 clock 밖에서
검사했다. 아래 변화는 전후 표본 중앙값의 비율이며 음수는 시간 감소다.

| 후보 83fe33a8의 대표 경로 | 전후 시간 변화 | 결정 근거 |
|---|---:|---|
| GCC native, 96 명령, 변경 없는 코드 재검증 | −37.98% ~ −38.50% | MMU off/on 모두 7/7쌍 개선 |
| Clang native, 같은 재검증 | −35.25% ~ −36.79% | MMU off/on 모두 7/7쌍 개선 |
| Clang portable, warm control 8개 | +12.28% ~ +22.41% | 8개 경우 모두 7/7쌍 느려짐 |
| GCC, 2명령 변경 코드 재번역 control | +12.19% ~ +16.28% | 여러 짧은 경로에서 회귀 |

후보는 비트맵 함수 단독 비용이 아니라 notification과 dispatch를 포함한 pipeline의
측정 대상이다. 유리한 일부 수치만으로 공통 최적화에 채택하지 않았다. 코드 배치나
compiler codegen에 따른 회귀의 상세 원인은 확정하지 않았으며, 이번 작업에서
새 최적화를 더 만들지 않았다. 전체 96개 표와 pair별 값은 `audit-invalidation/TIMINGS.md`,
`timing-summary.json`, `timed-samples.jsonl`에 있다. 게임·Windows Princess·H700의
성능 변화로 외삽하지 않는다. 이 후보의 성능 변화는 최종 패치의 속도 향상 주장이 아니다.

### 장치 이벤트 순서 보정의 비용

최종 41a86b7의 공통 코어에 scheduler 변경만 적용/제외한 두 실행 파일을 비교했다.
ARM 코드는 양쪽 모두 기준 구현이며, DMA STOP과 나머지 수정도 양쪽에 같이 들어
있다. GCC 13.3, `-O3`, C23, profiling/IPO OFF, AMD EPYC 9V74의 허용된 CPU 0에
고정했다. 다른 빌드·테스트가 없는 구간에서 10개 case/mode마다 6쌍을 교대로
실행했다. calibration 10개와 실제 표본 120개, 총 130개 기록을 보존했다.

아래 절대 비용은 각 쪽 표본의 중앙값이다. 변화율은 각 전후 pair 비율의 중앙값으로
계산했으므로, 표시된 절대 중앙값으로 다시 계산한 비율과 약간 다를 수 있다.
portable은 native JIT를 끈 모드이며, 기존 portable 번역 블록과 안정 루프 생략을
쓴다. CPU의 `B .` 입력은 scheduler 호출 비용을 보기 위한 합성 입력이다.

| 동일 작업량의 장치/CPU 경로 | 기준 → 수정 후 | paired 변화 | 느려진 쌍 |
|---|---:|---:|---:|
| 조용한 32,768-cycle raw tick | 3.908 → 5.291 ns/call | +35.65% | 6/6 |
| 같은 구간, portable | 53.184 → 58.505 ns/call | +9.66% | 6/6 |
| 같은 구간, native JIT | 53.110 → 58.128 ns/call | +9.55% | 6/6 |
| 이미 pending인 PWM, IIS/LCD off, UINT32_MAX tick | 16.563 → 20.159 ns/call | +22.22% | 6/6 |
| 보통 장치 부하, raw tick | 49.867 → 50.381 µs/frame | +1.41% | 5/6 |
| 보통 장치 부하, portable | 51.636 → 52.179 µs/frame | +1.15% | 6/6 |
| 보통 장치 부하, native JIT | 51.722 → 52.045 µs/frame | +0.83% | 4/6 |

보통 장치 부하는 약 42.97 kHz RAM→IIS FIFO, 1 kHz PWM, 60 Hz 240×320 TFT이며
host chunk를 한 프레임인 800,000 cycles로 맞췄다. 이 일곱 행은 레지스터·phase·
CPU 관찰·PCM·픽셀·render 수·전체 RAM 등 기록한 값이 전후 일치하고 두 oracle도
통과했다. 조용한 raw tick은 사전에 정한 1천만 반복 상한에 닿아 목표 150 ms보다
짧은 약 38–53 ms 구간이었다. 표본과 이 제한을 그대로 남겼다.

조용한 경로의 상대적 회귀도 존재한다. 절대 증가량은 해당 네 microbench에서
call당 약 1.38–5.32 ns이고, 보통 장치 부하의 합성 프레임에서는 약 0.32–0.54 µs다.
이 비용을 숨기거나 실제 게임 속도와 같다고 보지 않는다. 관찰 순서의 재현 결함을
해결하는 변경이며, 게임에서의 필요 빈도와 전체 비중은 별도 측정 대상이다.

PWM의 MMIO count를 IIS로 읽는 96 kHz 경로는 기준 출력 자체가 잘못되므로
전후 속도 비율을 계산하지 않았다. 수정 후 oracle에 맞는 PCM 1 frame당 비용은
raw 70.90 ns, portable 144.81 ns, native 145.52 ns였다. 32,768-cycle host chunk당
각각 4.646/9.491/9.537 µs다. 기준 쪽 PCM 불일치는 각각 2,098,135/2,098,135,
1,039,042/1,039,073, 1,034,651/1,034,682 frames였고 수정 후에는 모두 일치했다.

한 public tick에 관찰 가능한 정수 cycle 경계가 E개 있으면 wrapper의 ordered
advance는 최대 2E+1회다. 이미 pending인 PWM, 안전한 RAM→고정 FIFO는 불필요한
sample별 경계를 만들지 않는다. IIS/LCD를 끈 pending-PWM UINT32_MAX fixture는
하나의 bulk advance와 고정된 timer scan을 유지한다. IIS가 켜져 있으면 기존 IIS
sample 처리 비용이 별도로 있을 수 있으므로 모든 큰 tick이 상수 시간이라는
주장으로 확대하지 않는다. 원시 기록과 command/affinity/hash는 `audit-events/cost/`에 있다.

## 상태 메모리 감사와 남긴 과제

기준 코어의 세 RAM/card 시나리오에서 24개 할당 실패를 주입했으며 모두 전체
직렬화 상태·host speed·추적 중인 live allocation을 보존했다. 각 시나리오에서
16회 반복 load 후 기기를 해제한 추적 live bytes도 0이었다.

| 기준 코어 load 시나리오 | 작업 중 추가 peak 요청 bytes |
|---|---:|
| RAM 8 MiB → 8 MiB, 카드 없음 | 9,286,542 |
| RAM 8 MiB → 16 MiB, 작은 전체 카드 | 17,684,124 |
| RAM 16 MiB → 8 MiB, 4.125 MiB delta 카드 | 9,287,069 |

이는 요청한 allocation bytes 검사이며 allocator overhead나 RSS가 아니다.
최종 WASM의 별도 공개 API 검사에서는 9,290,818-byte v17 파일을 32회 연속
저장했을 때 heap가 38,340,976 bytes로 유지됐다. 입력을 새로 할당해 load/save를
32회 반복하면 38,340,976과 46,729,600 bytes 사이에서 번갈아 유지됐다. 뒤의
16회가 앞의 16회보다 더 큰 상한을 만들지 않았고, 이어진 다섯 PCM/SoC 잘림
거절도 같은 상한 안에서 현재 상태를 완전히 보존했다. 이 합성 입력·횟수에서
관측한 경계이며, 모든 게임이나 브라우저 프로세스 전체 메모리의 상한은 아니다.

같은 RAM 크기의 buffer load에서 입력을 빌려 staging RAM 할당을 줄이는 방안은
이번 수정에 넣지 않는다. 입력이 live RAM/card와 겹치는 경우의 복사 순서와
frontend가 보관한 tail의 수명을 먼저 증명해야 한다. 현재의 실패 보존을 유지한
채 별도 alias·수명 테스트와 함께 검토할 수 있는 후속 과제다.

실제 게임에서 새 v17 저장 후 연속 PCM 재생, 되감기·run-ahead, pause/resume,
Windows/Mac/H700의 성능과 화면·오디오는 로컬 검증 대상으로 전달한다.

DMA의 기존 TSZ[27] 4-unit burst 모델은 generic/IIS 양쪽에서 구현되지 않은
별도 과제로 확인했다. 이번 STOP 검사는 기존 byte/halfword/word 전송 단위의
취소를 다루며, 기존 TSZ 동작을 실기 하드웨어의 정답으로 인정하는 검사가 아니다.
