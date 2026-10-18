# GP32emu 1.1.0 네 번째 검토: JIT 내부 분기, Thumb 전송, 주변장치와 상태 경계

검토일: 2026-10-18 UTC. 직전 공개 기준은
[`b141220cd5fd9d628305133fc963ed86a30f7e41`](https://github.com/gajae1/gp32emu/commit/b141220cd5fd9d628305133fc963ed86a30f7e41)이다.
이번 문서는 [첫 검토](REVIEW-1.1.0.md), [하드웨어·오디오 추가 검토](REVIEW-1.1.0-FOLLOWUP.ko.md),
[버스·IRQ·저장 심층 검토](REVIEW-1.1.0-DEEP.ko.md)에 이어지는 **새 변경과 새 증거**를 기록한다.
이전 문서의 40/44/49개 테스트 수와 성능 수치는 당시 코드의 기록으로 유지한다.

제품 소스는 로컬 `06c4d879543f90d1003d174718d738e27a8ed6b8`에서 확정했고, 같은 트리의
공개 커밋은 [`aa54dfc`](https://github.com/gajae1/gp32emu/commit/aa54dfc0f2b63b7d81e8c68be249ecc7431304d6)다.
두 기존 회귀 fixture의 bus 설정까지 바로잡아 최종 네 matrix를 실행한 기준은 로컬
`eebb337b7133fa19b330fe82fd9f002fa215b8b4`, 동일 트리의 공개
[`af387ea`](https://github.com/gajae1/gp32emu/commit/af387eaeb0687b250e8815a1795cb88b709195ee)다.
검증 트리는 `dd3b0d94445c3053d95faba2d59fd71fd7d9df65`다. 이후 변경은 문서·증거와
Switch CI의 중복 source argument 제거이며, 검증한 제품·test source bytes는 유지한다.
[commit 대응표](benchmarks/1.1.0-round4/evidence/commit-map.json)는 13개 code/test 단계의
로컬·공개 트리가 각각 일치함을 기록한다.

이번에 기본 코드에 채택한 성능 변경은 **x64 JIT의 블록 내부 조건 분기**, **Thumb LDMIA/STMIA의
선택된 레지스터 순회**, **HLE RAM 일괄 초기화**다. 함께 재현한 버스 fetch, ARM 제어 인코딩,
DMA 요청, IRQ 우선순위, 외부 코드 쓰기, 상태 복원 경계의 결함도 수정했다.
속도가 느려진 조건과 미채택 후보를 원시 측정과 함께 남겼다.

여기서 측정한 시간은 공유 x86-64 호스트의 합성 workload 시간이다. 실제 게임 실행 빈도,
프레임률, 전력, Android/iOS/레트로 기기의 성능으로 환산하지 않는다. AArch64 QEMU 결과는
생성 코드의 실행과 상태 일치 검증에 사용하며 물리 AArch64 성능 근거로 사용하지 않는다.

## 1. 이번 단계의 결과와 범위

| 대상 | 재현 또는 측정 결과 | 결정 |
|---|---|---|
| x64 조건부 전방 B | 블록 내부 이동으로 dispatcher 왕복 감소; taken 경로 9.1~37.4% 시간 감소 | 채택, 미성립 분기·코드 생성 비용 명시 |
| Thumb LDMIA/STMIA | 두 루프에서 선택된 비트만 순회; 12개 중앙값 모두 개선 | 채택, GCC/Clang 측정 범위 명시 |
| HLE zero | 큰 초기화의 byte callback 제거; 이미 번역된 명령도 무효화 | 채택, 4바이트 비용 증가 명시 |
| BIOS 직접 읽기 창 | 1바이트 승인으로 512 KiB 전체를 직접 읽던 계약 위반 | 완전한 span 승인으로 수정 |
| callback 기반 opcode fetch | 미리 읽기·재읽기·cached 실행의 읽기 누락, fetch 중 상태 변경 | 이미 시작된 fetch와 명령 완료 경계 보존 |
| ARMv4T 제어 공간 | BXJ 등 미할당 인코딩이 PSR 경로에 진입 | 허용되는 MRS/MSR/BX 중심의 allocation 검사 |
| 소프트웨어 DMA | ON만으로 실행, 반복 SW_TRIG 누락 | enable/reload와 실제 요청 분리 |
| IRQ PRIORITY | 레지스터를 무시하고 낮은 IRQ 번호만 선택 | 계층 arbiter와 선택 경로 rotation 구현 |
| EEPROM/ID HLE 출력 | RAM에 쓴 새 코드 대신 이전 decoded 명령 실행 | 쓰기 후 외부 코드 변경 알림 |
| 상태의 framebuffer 크기 | 고정 저장소보다 큰 높이/너비 수용 | 상태 적용 전에 범위 검증 |
| HLE application query | 마지막 유효 word 거절, uint32 wrap 주소 승인 | 공통 bounded RAM span 사용 |
| 저장/실패한 load | 비정수 CPU 배속에서 주변장치의 진행률 손실 | live clock phase를 정확히 복구 |
| A64 두 번째 lookup way chaining | 기능·구조 검사 통과, 물리 성능 미측정 | 선택 패치로 보관 |
| PWM 현재 카운터/버퍼 | 다섯 타이머 모두 manual update/stop/reload 모델 차이 재현 | 미수정 재현 자료와 다음 설계 과제로 보관 |

새로운 제품 변경은 작게 나누고, 기능 검사와 성능 실험을 구분했다. 테스트의 시나리오 수,
실패 assertion 수, native 실행 event 수는 서로 다른 단위다. 많은 입력을 통과했다는 사실을
많은 버그를 고쳤다는 뜻으로 세지 않는다.

이번 집중 검토는 `arm920t.c`, x64/A64 emitter, bus window와 코드 invalidation, `gp32.c`의
HLE·저장 호출, `s3c2400.c`의 DMA/IRQ/PWM/IIS/LCD·상태 경계다. 공통 오디오 FIR,
SmartMedia·loader·codec·media·libretro·iOS C bridge·WASM은 기존 검토 기록과 연결 경로,
최종 회귀 실행으로 다시 확인한다. Java/Qt C++ UI를 일괄 재작성하거나 SDK 의존성을 갱신하지 않았다.
모든 파일의 모든 동작을 실리콘과 대조한 검증이라는 뜻은 아니다.

## 2. x64 JIT: 블록 안의 조건 분기를 실제 native 분기로 유지

파일: [ARM/JIT 코어](../src/arm920t.c), [A64 emitter](../src/arm920t_jit_a64.inc),
[JIT 회귀 검사](../tests/arm_jit_test.c).
측정 전체: [x64 forward branch 보고서](benchmarks/1.1.0-round4/x64-forward.md).

### 어떤 비용을 없앴는가

투명색을 건너뛰는 복사와 비슷한 코드는 값을 읽고 비교한 뒤, 조건이 맞으면 몇 개의 store나
계산을 지나 다음 반복 제어로 이동한다. x64 emitter는 이전에 이 조건 분기가 성립하면
native block을 반환하고 dispatcher가 목적지 block을 다시 찾도록 했다.
목적지가 이미 같은 직선 trace 안에 있을 때도 이 경로를 탔다.

새 코드는 조건부 `B`의 목적지와 그 사이 명령이 연속 주소이고, link·stop·trace 경계를 넘지
않으며, poll accounting을 건드리지 않는 경우에만 generated block 내부로 점프한다.
A64에 있던 eligibility helper를 의미 변경 없이 공통 위치로 옮겨 두 emitter가 함께 사용한다.
호출·return tracing이나 임의의 control-flow graph 최적화로 범위를 넓히지 않았다.

### 건너뛴 명령의 retirement와 callback 뒤 복귀

단순히 host jump만 넣으면 반환한 guest instruction 수가 틀린다. 디코딩된 prefix 길이에는
실행하지 않은 명령도 포함되기 때문이다. 이미 prologue에서 보존하던 R15D에 음수 skip 수를
누적하고, 모든 반환에서 `prefix + negative_skips`를 계산한다. 실제 skip이 있는 block만
초기화 비용을 부담한다. 부족한 entry budget에 대한 기존 zero return은 그 초기화보다 앞에 있다.

이는 instruction count를 줄이는 최적화가 아니다. guest가 실제로 실행한 수와 cycle budget을
유지하면서 host dispatcher 호출만 줄인다. block 입장 예산은 전체 길이를 수용해야 하므로,
분기 후 짧아진 경로에 별도의 느슨한 deadline 가정을 추가하지 않는다.

분기 합류점에서는 EAX의 전달 값만 믿을 수 없으므로 기존 guest register forwarding을 비운다.
합류점 가운데로 진입할 수 있는 상수 접기는 중지한다. helper callback이 flags, PC, trace,
generation 또는 실행 한도를 바꾸고 반환하는 경로에서도 skip 수가 정확히 반영된다.
미해결 branch patch는 publish하지 않고 compilation을 실패 처리한다.

profiling도 고쳤다. native 반환 수가 decoded prefix index가 아닌 block에서는 그 수를
`ops[done-1]`의 opcode로 잘못 분류하지 않는다. A64의 loop뿐 아니라 양쪽의 forward skip도
이 metadata에 포함한다. profiling을 끈 제품 구성에는 이 배열이 없다.

### 기능 검증과 실제 비용

새 검사는 조건/NZCV, 다음 명령 분기, 작은 budget, 여러 skip, MMU 매핑 유무, 상수 접기와
forwarding의 합류점, flags를 소비하는 callback, 들쭉날쭉한 실행 예산을 다룬다.
512개 조건·직접 entry 사례, 16개 합류 사례, 9개 callback 사례와 별도 반복 workload를 사용한다.
최종 통합 검사에서도 기존 JIT differential 및 translation/invalidation 검사를 함께 실행한다.

profiling을 끈 `-O3` 빌드로 두 컴파일러 각각 17조건 × 7 AB/BA 쌍을 실행했다.
전체 **476개 표본**을 보관했다. 분기가 실제로 성립하는 9조건 × 7쌍 × 2컴파일러의
**126쌍이 모두 개선**됐다. 투명도 25/50/100%, MMU on/off, skip 길이 1/2/4/8을 포함한다.

| 관찰 | GCC 13.3 | Clang 18.1.3 | 해석 |
|---|---:|---:|---|
| taken 경로 | 9.61~36.97% 감소 | 9.08~37.40% 감소 | 이 synthetic 루프의 host 비용 감소 |
| eligible이나 분기가 전혀 성립하지 않음 | -0.75%, +0.53% | +0.98%, +1.67% | 진입/반환의 추가 accounting 비용 존재 |
| eligible block 반복 collection+emission, arena 유지 | +5.87% | +8.51% | 코드 생성 비용 증가 |
| 동일한 no-forward cold 대조군 | -1.00% | +5.00% | 컴파일러 배치·공유 호스트 변동도 존재 |

실행 workload의 원래 단위는 pixel당 ns다. collection+emission은 MMU off에서 두 번 warmup
후 entry를 폐기하고 5,000번 다시 수집·생성한 compilation당 ns이며 arena allocation은 유지한다.
첫 arena 할당, process startup, application launch 비용을 측정한 결과가 아니다.

모든 no-forward native 대조군의 중앙값 이동은 -2.69~+0.33%였다. 같은 대조군의 generated
code 크기 530바이트(MMU off), 790바이트(MMU on)는 유지됐다. 대조군의 변동까지
새 알고리즘의 이득으로 세지 않는다. 작은 검증 workload에서는 50% skip 시 native call 수가
3,072에서 2,048로 줄었으며, guest의 최종 RAM·레지스터·retirement는 동일했다.

채택 이유는 native 경로를 반복 실행할 때의 감소가 일관되고, 적용 조건과 비용이 한정돼 있기
때문이다. x64 결과를 사용자의 AArch64 휴대기기 개선율로 설명하지 않는다. A64에는 이와
같은 내부 전방 분기 기능이 이미 있었으며, 이번 공통화가 그 기기의 새 속도 향상을 증명하지 않는다.

## 3. 인터프리터: Thumb의 선택된 레지스터만 순회

파일: [ARM/Thumb 코어](../src/arm920t.c), [Thumb transfer oracle](../tests/arm_thumb_transfer_test.c).
측정: [전송 비교와 원시 데이터](benchmarks/1.1.0-round4/README-thumb-transfers.md).

Thumb LDMIA/STMIA는 기존에 r0부터 r7까지 매번 bit test를 했다. 이제 기존 `arm_ctz16`로
가장 낮은 선택 비트를 얻고 `regs &= regs - 1u`로 지운다. 희소 목록에서는 없는 레지스터를
검사하는 일을 줄이고, 실제 전송은 낮은 레지스터·주소 순서를 유지한다.

최종 writeback에 사용하는 원본 목록은 보존한다. 전송 주소, 정렬 정책, base가 목록에 있을 때의
정의된 동작, callback 순서, IRQ/stop을 받은 뒤 현재 명령을 완료하는 경계는 변경하지 않는다.
PUSH/POP와 raw ARM 전송은 이번 두 루프 변경의 대상이 아니다.

독립 oracle은 기존 1,030개 PUSH/raw ARM 사례에 STMIA 5,086개와 LDMIA 8,162개를 더해
**14,278개**를 검사한다. 정의된 nonempty 목록, 여덟 base register, 주소 하위 두 비트,
첫 bus callback에서의 stop/IRQ를 포함한다. 정의된 동작에 대해서는 수정 전·후 모두 통과한다.
이번 전송 변경은 정확성 결함을 고쳤다는 주장이 아니라, 같은 동작을 더 적은 순회로 구현한 것이다.

| register mask | GCC 시간 변화 | Clang 시간 변화 |
|---|---:|---:|
| `01` | -20.44% | -18.43% |
| `80` | -17.30% | -14.88% |
| `81` | -14.52% | -12.64% |
| `55` | -10.99% | -9.38% |
| `7f` | -9.59% | -5.40% |
| `ff` | -8.74% | -4.69% |

단위는 base 복원과 loop branch까지 포함한 guest instruction당 host 시간이다.
336개 전체 표본 중 Thumb의 84쌍에서는 83쌍이 개선했고, GCC `ff` 한 쌍은 +0.82%였다.
이 표본도 제거하지 않았다. 변경하지 않은 raw ARM 대조군은 GCC -0.98~+1.28%,
Clang -7.23~-2.30% 움직였다. 컴파일러의 코드 배치와 호스트 변동이 함께 작용할 수 있으므로
raw ARM 자체가 개선됐다고 부르지 않는다.

GCC/Clang의 builtin ctz는 0에 호출하면 안 되지만, 모든 새 호출은 `regs != 0` loop 안이다.
다른 컴파일러의 기존 C fallback은 유지한다. 그 fallback은 선택마다 bit 0부터 다시 검사하므로
MSVC 등에서 같은 성능 이득이 있다는 증거는 없다. 단지 C23이라는 이유로 `<stdbit.h>`를
필수화하지 않았다. 지원 중인 툴체인의 C23 라이브러리 가용성은 dialect 선택과 별개다.

### 명세가 불명확한 값을 성급히 고치지 않은 사례

Thumb STMIA의 base가 목록의 첫 레지스터가 아닌 경우에 대해 Samsung의 일부 설명과
일반 ARM 문서의 표현이 달랐다. ARM ARM A7-97은 해당 저장 값을 UNPREDICTABLE로 다룬다.
실리콘 결과 없이 특정 값 하나를 정답으로 고정하면 테스트를 통과시키면서 호환성을 바꿀 수 있다.
그 초기 실험은 채택하지 않았고 새 oracle에서도 그 값의 비교를 제외했다.
이를 재현된 ARM920T 결함이나 성능 성과로 세지 않는다.

## 4. ARM 제어 공간: DSP 다섯 명령보다 넓은 allocation 검사

파일: [shared ARM validator](../src/arm920t.c), [exception 검사](../tests/arm_exception_test.c).

이전 단계는 `SMLAxy`, `SMLAWy`, `SMULWy`, `SMLALxy`, `SMULxy` 등 알려진 ARMv5 확장
마스크를 개별 거절했다. 이번에 control extension space 전체를 대조하니 그 방식은 구멍을
남겼다. 예를 들어 `BXJ r0` 인코딩 `0xe12fff20`이 MSR 경로에 들어갔고, r0 값에 따라
CPSR mode와 banked state를 바꿀 수 있었다. 예외를 내지 않고 잘못된 PSR 작업을 한 것이다.

제어 공간의 바깥 조건을 먼저 식별하고 ARMv4T의 MRS, register/immediate MSR, BX에
해당하는 allocation만 허용하도록 바꿨다. 기존 zero-field MSR 정책이나 operand 수준의
UNPREDICTABLE 사례까지 새로 정의하지는 않는다. 조건이 거짓이면 기존 ARM 조건 실행
규칙에 따라 그 명령을 실행하지 않는다.

75개 allocation cell × 세 operand 배치 × 세 실행 경로 × 네 실행 phase,
**2,700개 사례**를 명령 마스크 구현과 별도로 구성했다. 최종 oracle로 수정 전
**4,874개 assertion**이 실패했고, 수정 후 통과했다. 정상 PSR/CPSR·exception-return도
positive control로 계속 검사한다.

근거는 [ARM ARM DDI0100I](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a)
A3.16.3/Table A3-5와 [ARM920T TRM DDI0151C](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb)
2.2.2다. 이는 ARM920T가 ARMv4T이며 뒤 ISA에 할당된 명령을 일반 PSR 명령처럼
실행하면 안 된다는 근거다. 사용자가 가진 게임 이미지에 해당 bit pattern이 있는지,
그것이 실행되는 명령인지까지 확인한 것은 아니다.

GP32의 별도 audio DAC가 있다는 사실은 CPU에 ARMv5TE DSP 명령을 추가하지 않는다.
이 부분의 S3C2400/IIS·DMA, UDA1330A 자료와 회로도 확보 범위는
[이전 하드웨어 검토](REVIEW-1.1.0-FOLLOWUP.ko.md#2-별도-오디오-칩과-회로-자료)를 참조한다.
새 전체 GP32 board schematic이나 실게임 opcode 빈도 자료를 확보했다고 주장하지 않는다.

## 5. JIT와 공통 실행의 bus 계약

### BIOS 직접 읽기 창을 1바이트 승인으로 만들던 문제

`arm_jit_refresh_bus_windows`는 `fastmem(c, 0, 1, 0)`이 non-NULL이면 이를 512 KiB BIOS
전체의 base로 저장했다. bus가 처음 64 KiB만 직접 노출하는 경우에도 나머지 읽기를 callback
없이 처리할 수 있었다. 공개 bus의 `fastmem` 계약은 요청한 **전체 span**에 대한 승인이다.

요청 길이를 실제 cached window의 길이로 바꿨다. 일반 SoC가 완전한 BIOS span을 제공할 때의
읽기 hot path에는 매번 bounds 검사하는 분기를 추가하지 않는다.
부분 direct window는 기존 정책에 따라 native 요청이 실제 native 실행으로 이어지지 않을 수도
있으므로 회귀 검사에서 요청과 실행 확인을 구별한다.

[새 검사](../tests/arm_read_window_test.c)는 partial/full window, flush/reset/state apply/
재설정, word/rotated/byte/half/signed/LDM 읽기의 주소·값·횟수를 검사한다.
10개 구성에서 수정 전 220개 assertion이 실패했고 수정 후 통과했다.

### opcode fetch callback은 부작용 없는 미리보기가 아니다

`fastmem`이 NULL인 instruction 영역은 읽기 callback을 통해 fetch해야 한다. 이 callback은
MMIO 같은 관찰 가능 경로일 수 있고, PC/CPSR·trace·실행 한도를 바꾸거나 JIT를 flush할 수도 있다.
기존 block collector는 미래 opcode를 미리 읽고, cached 실행은 필요한 callback을 생략했다.
처음 fetch 중 flush가 발생하면 이미 읽은 word를 버리고 다시 읽는 경우도 있었다.
MMU의 cold table walk callback이 PC/Thumb/limit를 바꾸면 진행 중이던 ARM fetch가
유실되거나 다른 실행 상태로 넘어갈 수 있었다.

이제 미래 collection은 부작용 없는 peek가 가능한 경우에만 한다. 코드의 캐시 가능성을
direct pointer와 RAM/BIOS 물리 span으로 증명하고, 이미 시작된 callback 기반 fetch의
반환 word를 보존해 그 callback을 재호출하지 않는다. 안정적인 direct memory까지 decode
과정에서 단 한 번만 읽는다는 보장은 아니다.
fetch를 시작한 PC/ARM 상태와 generation/deadline/control을 기록하며, callback이 그 조건을
바꾸면 취득한 ARM word를 기존 `exec_arm_at` 경로로 한 번 완료한다.
hot cached block hit마다 같은 snapshot 비용을 새로 넣는 변경은 아니다.

[회귀 검사](../tests/arm_fetch_callback_test.c)의 **162개 시나리오**는 세 MMU 상태,
두 dispatcher 설정, 여러 budget과 callback 효과를 포함한다. flush/limit/stop/PC/flags/
mode/Thumb/trace/IRQ/FIQ와 table-walk 효과를 검사하고 이어지는 다섯 명령의 결과도 비교한다.
callback 수·순서·instruction completion과 후속 CPU 상태를 함께 관찰한다.

최종 같은 oracle을 공개 기준 `b141220`의 정확한 `arm920t.c`와 실행한 보관 로그에서는
**4,281개 assertion 실패**, 현재 수정에서는 0이다. 개발 도중의 더 작은 fixture 숫자와
최종 baseline 숫자를 혼합하지 않는다. full-direct native positive control도 포함한다.

## 6. HLE가 RAM을 바꿀 때: 성능과 code coherence

### BSS·PCM 초기화를 한 번의 bounded span으로 처리

`direct_zero_if_ram`은 유효 범위가 RAM에 완전히 들어가는지 확인하고, bus가 그 전체 writable
span을 제공하면 `memset`을 사용한다. direct span을 제공하지 않으면 기존 byte callback
fallback으로 쓴다. 어느 쪽이든 쓰기가 끝난 뒤 CPU에 instruction range 변경을 알린다.

기존 byte write loop에는 이 알림이 없어서 이미 실행한 MOV를 0으로 덮어도 cached/native
경로가 옛 MOV를 다시 실행할 수 있었다. [71개 HLE memory 검사](../tests/gp32_hle_memory_test.c)는
경계·길이·실패 원자성과 실제 CP15 MCR을 통한 I=0/1 코드 덮어쓰기를 다룬다.
수정 전 네 stale-code assertion이 실패했고 이후 통과했다. 단순히 0이 된 RAM만 비교하지 않았다.

성능은 profiling OFF, 같은 다른 core object를 링크한 두 `gp32.c` 변형, 7 AB/BA쌍,
9개 크기와 1/8개 working span, 두 컴파일러 **504개 표본**으로 측정했다.
64 warmup 뒤 이미 0인 resident memory를 반복 초기화하는 helper 비용이다.

| clear 크기 | GCC 시간 변화 | Clang 시간 변화 | 의미 |
|---|---:|---:|---|
| 4 B, 한 span | +13.20% | +38.53% | 작은 호출에서는 새 무효화·span 경로 비용이 큼 |
| 8 B | -34.31% | -22.66% | byte callback 반복 제거 |
| 64 B | -90.79% | -89.79% | 동일 |
| 4 KiB | -99.29% | -99.28% | bulk zero가 대부분의 기존 dispatch 비용 제거 |
| 512 KiB | -99.44% | -99.44% | 반복 초기화 kernel 시간; 게임 배속 아님 |

4바이트는 두 working set을 합쳐 약 1.1~3.2 ns, +11.4~38.5% 느려졌다. 수정 전이 필요한
code invalidation을 생략했다는 차이까지 포함한다. 이 숫자에 맞춘 tiny-size threshold를
추가하지 않았다. 대상 기기에서 호출 길이 분포와 알림을 유지한 baseline을 비교해야 한다.
전체 표와 재구성 방법은 [HLE zero 보고서](benchmarks/1.1.0-round4/README-hle-zero.md)에 있다.

### EEPROM/ID를 읽어온 뒤 남는 옛 명령

SWI 0x104의 16바이트 ID 출력과 0x105의 EEPROM→RAM 출력에도 같은 외부 코드 변경
알림이 빠져 있었다. [EEPROM 검사](../tests/gp32_eeprom_test.c)는 세 실행 경로 × I=0/1 ×
두 출력, **12개 시나리오**를 실행한다. 실제 SWI/MCR과 primed code를 사용했으며
수정 전 여덟 stale-code assertion 실패가 수정 후 사라졌다.
RAM에서 EEPROM으로 쓰는 방향에는 guest RAM 변경 알림을 추가하지 않는다.
untracked data와 EEPROM-write의 invalidation counter도 대조한다.

모든 SoC write에 무조건 알림을 붙이지 않았다. CPU의 I-cache 모델, DMA의 외부 쓰기,
HLE의 외부 쓰기는 현재 코어가 정한 코드 coherence 경계에 맞춰야 한다.
나머지 HLE scalar output helper들의 일관된 gateway 여부는 후속 점검 후보이며,
이번에 재현하지 않은 경로까지 고쳤다고 주장하지 않는다.

### application query의 마지막 word와 wraparound

SWI 0x0f fallback의 `argp + 4 < RAM_END`는 마지막 유효 word를 거절했고, uint32 overflow가
발생하는 높은 주소를 잘못 승인했다. 공통 `direct_ram_range(..., 4)`를 사용해 완전한 word
범위로 판단하도록 바꿨다. [SWI 검사](../tests/gp32_swi_1ff_test.c)에 13주소 × 세 실행 경로,
**39개 시나리오**를 추가했다. 64-bit 범위 oracle과 전체 RAM, PC/CPSR/cycle을 비교한다.
수정 전 18개 assertion 실패가 수정 후 통과했다.

## 7. DMA와 IRQ: 빠른 RAM 경로 옆의 제어 레지스터

### DMA enable은 software request와 다르다

이전 코드는 ON이 올라가면 software DMA를 시작했고, 이미 ON인 channel의 다음 SW_TRIG는
실제 재요청으로 처리하지 않았다. ON rising edge는 current count/cursor를 initial 값으로
준비하는 동작으로 남기고, 실행 요청은 ON과 SW_TRIG가 동시에 유효하며 DCON.SWHW가
software일 때만 받도록 나눴다. 수락한 trigger는 self-clear하고 STOP의 우선순위도 보존한다.

[DMA 검사](../tests/s3c2400_dma_test.c)에 **72개 trigger 시나리오**를 더했다.
네 channel, byte/half/word, whole/single service, 반복 trigger, hardware/disabled,
ON only, STOP 조합, autoreload, 중간 저장/복원을 포함한다. 수정 전 324개 assertion 실패가
수정 후 통과했고, 이전 RAM DMA bulk-copy oracle 4,160개도 함께 유지했다.
근거는 [Samsung S3C2400 매뉴얼](https://datasheets.chipdb.org/Samsung/S3C2400.pdf)
8-10/8-13의 DCON·DMASKTRIG 설명이다.

### PRIORITY를 실제 IRQ 선택에 반영

이전 단계의 INTPND latch 수정은 선택을 acknowledgement까지 유지하게 했지만,
어떤 source를 먼저 선택할지는 여전히 낮은 bit 번호 순이었다. 이번에는 여섯 leaf arbiter와
상위 arbiter의 계층을 모델링했다. REQ0/REQ5의 끝 우선순위, 내부 REQ1~4의 SEL 순서,
MODE가 켜진 winning path의 rotation을 반영한다. PRIORITY reset은 0x7f다.

INTPND가 비어 있을 때만 새로 선택하므로 이미 선택된 IRQ를 바꾸거나 같은 IRQ의 재확인만으로
rotation을 반복하지 않는다. FIQ와 mask, pending acknowledge 경로를 별도로 유지한다.
구버전 저장 상태가 갖고 있는 PRIORITY=0은 고정 우선순위 값으로 그대로 해석한다.
새 필드를 저장 형식에 더하지 않았다. 하드웨어에서 write-only인 PRIORITY를 일반 readback
레지스터로 제공했다고 설명하지 않는다. 검사는 내부 저장 상태를 관찰한다.

[새 priority oracle](../tests/s3c2400_priority_test.c)은 literal priority 순서를 기준으로
**2,508개 시나리오**를 검사한다. 모든 실제 연결된 nonempty input 집합, SEL/MODE,
반복 service, 미선택 arbiter, held latch, mask/FIQ, rewrite와 save/restore가 포함된다.
수정 전 10,042개 assertion 실패, 수정 후 0이다.

Samsung 매뉴얼 14-5의 “serviced”를 이 코어에서는 **빈 INTPND에 수락하는 순간**으로
모델링했다. 문서에 따른 구현 해석이며 실리콘의 bus-cycle arbitration을 관측한 결과는 아니다.
더 많은 정확성 로직을 추가한 변경이므로 이 항목 자체를 CPU 성능 개선으로 분류하지 않는다.

## 8. 저장과 load 실패가 실행 중인 기계를 바꾸지 않도록

### 고정 framebuffer 저장소의 크기를 넘는 상태

SoC는 고정 240 stride와 240×320 저장소를 갖는데, 저장 parser가 읽은 `fb_w/fb_h`를
제한하지 않았다. 뒤의 이미지 변환은 그 metadata를 사용하므로 너무 큰 높이는 실제 배열을
벗어난 행을 참조할 수 있었다. 이제 상태를 적용하거나 새 allocation을 진행하기 전에
너비 1..240, 높이 1..320을 검사한다.

[상태 검사](../tests/s3c2400_state_test.c)는 zero, one-past, UINT_MAX의 여섯 잘못된 tuple이
전체 SoC 상태를 바꾸지 않고 거절되는지 확인한다. 1×1, 37×209, 240×320의 유효 상태는
metadata와 마지막 pixel sentinel을 보존한다. 위험한 out-of-bounds read를 실행시켜야만
문제를 재현하도록 만들지 않았다. 수정 전 12개 assertion 실패가 수정 후 통과했다.
확보된 저장 writer 이력은 이 범위를 사용하며 serialized layout/version은 그대로다.

### 비정수 CPU 배속에서 save/rejected load의 phase drift

저장은 machine clock을 잠깐 nominal 값으로 환산한 후 host의 CPU 배속으로 되돌렸다.
PWM/IIS/idle audio/LCD의 accumulator를 정수 비율로 두 번 바꾸면 나머지가 버려질 수 있다.
150%에서 guest 두 cycle 뒤 저장하거나 실패할 load를 시도하기만 해도 다음 timer count가
control의 99 대신 100이 되는 작은 재현을 확인했다. `state_size`와 100% 대조군은 달랐다.

내부 clock checkpoint에 다섯 PWM accumulator, IIS 진행률, idle audio phase, LCD line
accumulator와 HCLK remainder, host 배속을 담는다. save와 거절된 load가 끝나면 정상 speed
setter로 derived cache를 갱신하고, 원래 phase를 정확히 돌려놓는다. **성공한 load는 incoming
state의 phase를 유지**하며 host의 선택 배속만 정상 환산으로 복원한다.

이것은 공개 API나 파일에 새 상태를 저장하는 변경이 아니다. 일시 작업의 stack checkpoint이며
매 tick의 hot path에 추가 연산도 없다. derived cache가 정당하게 dirty가 되는 것까지
금지하는 byte-for-byte 내부 구조 보존을 주장하지 않는다. 보장 대상은 실행 진행률이다.

[새 state-speed 검사](../tests/gp32_state_speed_test.c)의 **52개 시나리오**는
100/133/150%, TFT/fallback LCD, 반복 buffer save·file save, 앞/뒤에서 실패하는 load,
file/buffer cross-speed 성공 load를 다룬다. 후속 ragged execution의 timer 다섯 개,
IRQ/LCD/PCM과 저장 결과를 비교한다. 수정 전 20개 시나리오에서 114개 assertion이 실패했고,
수정 후 통과했다. 상태·timing·callback·wait 통합 경로도 함께 검사한다.

## 9. 선택 패치와 확인했지만 남겨 둔 문제

### A64 block chaining의 두 번째 way

A64 runtime hop은 두 way block table의 첫 번째 way만 직접 확인했다. 두 번째 way에
있으면 dispatcher로 돌아가서 정상 실행하므로 이는 잘못된 guest 결과를 낸다는 결함이 아니다.
첫 tag/generation/epoch 검사 실패 시 두 번째 way를 확인하는 **7개 cold instruction,
28바이트** 후보를 만들었다. 기존 ready/native/poll/deferred/budget/IRQ/FIQ gate로 합류한다.
첫 번째 way hit가 성공하는 경로에 추가로 실행하는 instruction은 없다.

다만 miss에서는 추가 probe 비용이 든다. 실제 기기의 collision과 hit/miss 분포 및 branch
predictor 비용을 모르므로 기본 코드에 넣지 않았다. [선택 후보 보고서와 패치](benchmarks/1.1.0-round4/optional-a64-chain.md)에
최종 통합 소스에 적용하는 방법과 검증을 보관한다.

30개 검사는 실제 hash collision으로 way를 바꾸고 예산 1..1,000을 비교한다.
오래된 generation/epoch, invalid/native 없음, poll/deferred, 부족한 budget,
masked IRQ/FIQ, 잘못된 tag를 직접 native entry에서 확인한다. dispatcher가 table을 고쳐
잘못된 hop을 숨기는 검사가 되지 않게 했다.
초기 기준의 way1/budget1000은 251 native call/249 hop, 후보는 1 call/499 hop이었다.
둘 다 1,000 guest instruction과 architectural state가 같다. 이를 몇 배의 실제 속도 향상으로
변환하지 않는다. 선택 패치는 default 제품 검사와 별도로 최종 코어 위에서 재검증한다.

### PWM live counter와 TCNTB double buffer — 미수정

더 큰 새 정확성 과제는 PWM이다. 현재 구현은 진행률과 TCNTB를 통해 count를 유도하므로
동작 중 TCNTB를 바꾸거나 stop/manual update를 하면 현재 count까지 바뀔 수 있다.
매뉴얼 10-4~10-5의 double buffering·manual update·stop 보존과 맞지 않는 관찰을
다섯 타이머 모두에서 재현했다.

| 관찰 | 현재 모델 예 | 기대되는 분리 |
|---|---|---|
| 현재 90일 때 TCNTB를 200으로 기록 | 즉시 190으로 변경 | live count 90, 다음 reload만 200 |
| stop | 100으로 보임 | 현재 90 유지 |
| stopped tick 후 재시작 | 99로 진행 | 보존한 90에서 89로 진행 |
| manual update로 50 기록 | 40으로 보임 | 새 live count 50 |

[재현 C 파일](benchmarks/1.1.0-round4/evidence/pwm-model-gap.c)과
[25개 mismatch 로그](benchmarks/1.1.0-round4/evidence/pwm-model-gap.log)를 보관했다.
terminal count가 0을 포함하는지의 논쟁과 별개로 count 보존을 비교한다.
아직 고치지 않은 동작을 정상 release의 CTest 실패 항목으로 추가하지 않았다.

제대로 고치려면 타이머별 latched reload와 live count, manual update, stopped phase,
autoreload 시점을 명확히 나누고 저장 상태를 버전 확장해야 한다. 구버전 필드의 사용되지
않는 bit에 숨겨 넣으면 compatibility 의미가 불명확해진다. 이번의 phase checkpoint 수정은
이 live-counter 모델까지 해결하지 않는다. 다음 정확성 작업의 우선순위가 높은 별도 변경이다.

### FIR의 handwritten NEON — 아직 성능 가설

기존 SSE2 FIR은 유지했다. 이번에는 정상 AArch64 Release object를 실제로 disassemble해
불확실성을 더 좁혔다. GCC 13.3, `-O3 -DNDEBUG -std=c2x -fPIC`, 별도 `-mcpu/-march` 없는
구성에서 **contiguous source loop는 scalar SMADDL, planar history는 이미 NEON**이었다.
history는 8-tap group의 SMULL/SMULL2와 64-bit lane으로의 widening addition, TBL reversal,
ADDP reduction을 사용하며 4-tap vector와 1~3-tap scalar tail도 생성했다.

[assembly·정확한 compile command·object hash](benchmarks/1.1.0-round4/evidence/audio-a64-inspection/README-audio-a64-gcc13-06c4d879.md)를
보관했다. 이 object의 관찰은 Clang이나 특정 기기의 `-mcpu` 빌드 결과까지 대신하지 않는다.
새 후보는 이 혼합 baseline과 비교해 source loop나 history setup/unroll 비용을 개선할 수 있는지
확인해야 한다. handwritten NEON이 반드시 빠르다는 결론은 assembly만으로 나오지 않는다.

이전 scalar 방향 변경은 일부 약 6% 회귀, accumulator unroll은 혼합 결과, source-only SIMD는
작은 block에서 회귀했다. 이 기록을 지우고 같은 아이디어를 새 성과로 포장하지 않는다.
NEON 후보는 물리 A64에서 1/32/128/512/2048 frame, 여러 rate, source/history 비율을
나눠 7 AB/BA쌍으로 재는 것이 다음 실험이다.

정확성 조건도 분명하다. signed 16×16 product 하나는 int32에 들어가지만 `(-32768)^2`
두 개의 합 2,147,483,648은 들어가지 않는다. 32-bit pair 합을 만든 뒤 widen하면 잘못될 수
있으므로 충분히 일찍 64-bit 합으로 올려야 한다. L/R 분리, 역방향 source와 정방향 coefficient,
65 halfword/130-byte row의 비정렬 가능성, tail 경계를 유지해야 한다.
최종 `(acc + 16384) >> 15`와 clamp는 전체 합이 완성된 후 그대로 수행한다.
table 생성·phase·history·fade까지 한 실험에서 바꾸지 않는다.

## 10. C23과 최종 통합 검증

<!-- ROUND4_VALIDATION:START -->
네 CMake 구성은 모두 tests/libretro/headless/CPU profiling ON, IPO OFF,
`GP32EMU_REQUIRE_C23=ON`으로 설정했다. 실제 cache와 compile command,
54개 test inventory를 확인했다. 최종 결과는 다음과 같다.

| 구성 | C 표준과 진단 | 최종 결과 |
|---|---|---|
| GCC 13.3.0 x86-64 Release | `-O3 -DNDEBUG -std=c2x` | 54/54, 7.56초 |
| Clang 18.1.3 x86-64 Release | `-O3 -DNDEBUG -std=c23` | 54/54, 7.53초 |
| GCC 13.3.0 AArch64 Release + QEMU 8.2.2 | `-std=c2x`, 실제 A64 generated code 실행 | 54/54, 66.69초 |
| GCC 13.3.0 RelWithDebInfo + UBSan | `-std=c2x -fsanitize=undefined -fno-sanitize-recover=undefined` | 54/54, 8.84초, runtime 진단 0 |
| Clang/LLD 18 WASM + Node | C23 강제, 실제 WASM runtime | PCM gain·clock·VFS·반복 저장 통과 |
| 수정한 Switch CI 명령 + AArch64/QEMU mock | `-std=c2x -O2`, 전체 JIT differential | CodeMemory·권한 전환 두 모드 통과 |

표의 CTest 시간은 기능 검사의 wall time이며 속도 비교 수치가 아니다.
각 native backend의 실제 실행은 full JIT regression과 profiling으로 따로 확인했다.
ARM/Thumb 독립 oracle, 새 bus/IRQ/DMA/HLE/state 검사와 기존 audio/card/media/libretro 검사를
같은 source freeze에서 실행했다. Android/iOS SDK 앱, Win64 UI, 실제 게임이나 물리 기기는
실행하지 않았다.

UBSan의 전체 72개 compile command에 C23과 sanitizer 설정이 들어갔다. 최종 GCC UBSan
build에는 기존 경고 40회, 정규화한 위치 13곳이 있었고 모두 `b141220` 이전 blame 이력과
일치했다. 새로운 경고 위치와 runtime UBSan 진단은 없었다. 경고를 없애기 위해 무관한
제품 코드를 한꺼번에 바꾸지 않았다. 기존 경고의 실제 위치·내용도 증거에 보관했다.

첫 통합 검사에서는 두 기존 fixture가 실패했다. native control이 64 KiB BIOS만 할당했고,
literal/table callback을 BIOS 전체 direct 창과 겹치게 만들었다. 512 KiB 실제 backing,
literal만의 hole, BIOS/RAM 밖의 별도 table backing으로 고쳤다. native assertion이나
architectural 비교를 제거하지 않았다. fully callback-backed fetch 회귀도 그대로 통과한다.

GCC의 첫 재사용 build는 compiler 경로 `cc`→`gcc` 변경으로 CMake cache가 재생성되면서
tests 옵션이 OFF가 됐고, 남은 옛 CTest 파일의 49개 실행을 출력했다. 그 결과는 **최신 코드
검증으로 인정하지 않았다.** 새 build에서 옵션·54개 inventory를 명시적으로 검사한 결과가
위 표다. 실패한 최초 통합 기록, fixture 수정, 거절한 오래된 결과까지 구분해 보관했다.

WASM에서는 일정 PCM의 필터 gain, monotonic clock, VFS 성장/실패/clear 소유권과
9,290,562바이트 상태의 반복 저장을 검사했다. 반복 저장 후 heap은 38,340,656바이트로
재사용됐다. 이는 새 WASM 최적화 수치가 아니라 이전 수정이 현재 코어에서도 유지되는 검사다.

수동 Switch workflow에서도 링크 오류를 재현했다. `tests/arm_jit_test.c`가 이미
`arm920t.c`를 include하는데 compile 명령에 같은 코어를 다시 입력해 중복 정의 진단
45개와 exit 1이 발생했다. 이 중복 argument만 제거한 같은 flags의 AArch64 빌드는
exit 0이었고, CodeMemory와 `SWITCH_JIT_PERMISSION_MODE=1`의 전체 JIT differential이
각각 115,386 JIT event, fallback 0으로 통과했다. 이 수는 앞서 정의한 cache event 단위다.
[명령·전후 로그·source hash](benchmarks/1.1.0-round4/evidence/switch-ci/README.md)를 보관했다.
이는 Linux의 Switch memory-mapping mock 검사이며 libnx SDK/NRO 빌드, 물리 Switch나
원격 workflow를 실행한 결과는 아니다. 제품 소스·fixture·mock 구현은 변경하지 않았다.
<!-- ROUND4_VALIDATION:END -->

성능 표본은 모두 CPU profiling OFF로 측정했고, 기능 matrix는 native 실행 여부를 검사하기
위해 profiling ON이다. 두 구성의 숫자를 같은 측정으로 취급하지 않는다.
GCC 13의 `-std=c2x`와 Clang 18의 `-std=c23` 선택을 각각 기록한다.
`GP32EMU_REQUIRE_C23=ON`은 C11로의 조용한 fallback을 막는 gate다.
컴파일러가 최종 ISO C23의 모든 언어·라이브러리 기능을 구현했다는 인증이나, C23 지정만으로
성능이 좋아진다는 증거는 아니다.

재현할 때는 다음과 같이 C23 요구, tests와 native profiling을 명시한다.

```sh
cmake -S . -B build-round4 -DCMAKE_BUILD_TYPE=Release \
  -DGP32EMU_REQUIRE_C23=ON -DGP32EMU_BUILD_TESTS=ON \
  -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_BUILD_HEADLESS=ON \
  -DGP32EMU_CPU_PROFILE=ON -DGP32EMU_ENABLE_IPO=OFF
cmake --build build-round4 -j2
ctest --test-dir build-round4 --output-on-failure -j2

make -f Makefile.wasm build test GP32EMU_REQUIRE_C23=1 \
  WASM_CC=clang-18 WASM_LD=wasm-ld-18 -j2
```

## 11. 실험과 과거 작업을 재사용할 때의 기준

이번 시간 비교는 x64 476 + Thumb 336 + HLE zero 504 = **1,316개 표본**이다.
컴파일러·CPU affinity·warmup·반복 수·각 실행의 상태 검사·측정 source hash를 원시 자료와
함께 남겼다. 다른 agent의 build/test/QEMU를 멈춘 window에서 시간 측정을 수행했다.
공유 호스트 자체의 scheduling과 frequency 변동까지 제거한 것은 아니다.

독립 실험의 전후 코드는 나중에 다른 fix가 합쳐진 최종 파일과 다르다.
[source 재구성 patch](benchmarks/1.1.0-round4/isolation/)는 공개 `b141220`에서 각 실험의
정확한 source bytes를 만드는 용도다. 이미 적용된 최종 review branch에 추가 적용하는
최적화 patch로 쓰면 안 된다. 원시 JSON의 hash와 최종 통합 검사의 source/tree를 구별한다.

이전 기록의 framed leaf splice, TLB packing, paired loads, polling reuse와 private 기기 측정은
역사적 판단 자료다. 그 원래 게임·기기·F: 경로 로그를 이 환경에서 다시 실행한 것은 아니다.
직전 두 word-watch 후보의 mixed regression도 유지한다. profiling이나 컴파일러가 달라져
결론이 바뀔 수 있다는 점이 이번의 control·cold·untaken 비교를 포함한 이유다.

다음 물리 기기 단계에서는 느린 title마다 interpreter/native, I bit, MMU, native block 길이,
compile 수·time, dispatcher exit, audio 처리 비중을 먼저 확보하는 편이 좋다.
JIT kernel, HLE 초기화, DMA, audio FIR의 개선율은 서로 더할 수 없다.
기존 전체 시간에서 어떤 경로가 차지하는지에 따라 실제 체감은 크게 달라진다.

## 12. 근거와 다음 작업

| 다음 작업 | 필요한 증거 | 이번 상태 |
|---|---|---|
| PWM live/reload 분리 | independent event oracle, stopped/manual/autoreload, 구버전 저장 변환 | 재현 보관, 모델·state 설계 필요 |
| A64 두-way hop 채택 여부 | 실제 low-end A64 hit/miss workload와 cold/miss 비용 | 기능 검증 선택 패치 |
| A64 FIR NEON | source/history tiny block 포함 paired device 측정 | GCC object의 scalar source·NEON history 확인, 후보는 미구현 |
| x64 branch의 실게임 영향 | eligible/taken 빈도, cold code 비중, frame time 분포 | 합성 측정과 native 실행 검사 완료 |
| HLE 외부 write gateway | hot tick mirror와 실제 code overwrite 구분 | 일부 출력 수정, 나머지 별도 점검 후보 |
| ARM920T 모델 확장 | FCSE/AP/domain/fault/WFI/cache 동작의 재현과 CPU 명세 | 기존 제한 유지, 이번 범위 밖 |

주요 하드웨어 근거:

- [ARM ARM DDI0100I](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a): A3.16.3/Table A3-5, A7 LDMIA/STMIA.
- [ARM920T DDI0151C](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb): ARMv4T/undefined instruction, MMU와 bus 경계.
- [Samsung S3C2400](https://datasheets.chipdb.org/Samsung/S3C2400.pdf): DMA 8-10/8-13, PWM 10-4~10-5, IRQ 14-5.

결과를 확인할 파일:

- [측정·재현 자료 색인](benchmarks/1.1.0-round4/README.md)
- [기능 증거와 baseline 범위](benchmarks/1.1.0-round4/evidence/README.md)
- [최종 검증 metadata](benchmarks/1.1.0-round4/evidence/validation.json)
- [로컬·공개 commit/tree 대응표](benchmarks/1.1.0-round4/evidence/commit-map.json)
- [기존 Draft PR #1](https://github.com/gajae1/gp32emu/pull/1)
