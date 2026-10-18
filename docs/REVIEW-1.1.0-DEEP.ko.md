# GP32emu 1.1.0 심층 재검토: 버스 순서, 인터럽트, 저장 호환성과 최적화 근거

검토일: 2026-10-17~18 UTC. [첫 검토](REVIEW-1.1.0.md)와
[명령어·하드웨어·성능 추가 검토](REVIEW-1.1.0-FOLLOWUP.ko.md)를 잇는 문서다.
최초 검토 기준은 `1090ff174459c57d42106a028cf4e16b2f5fe618`이다.
직전 공개 체크포인트는 `f121769`, 이번 작업의 직전 로컬 체크포인트는 `5db6819`이며,
이 문서의 기본 제품 코드 기준은 **`a3e7d248814753edb8ec5ba454e32f730b19c019`**다.
그 코드와 트리가 같은 공개 커밋은
[`252db3e`](https://github.com/gajae1/gp32emu/commit/252db3e1863035b407783021d611d1bb3c521886)다.
아래 통합 커밋 번호는 로컬 기록이며, [공개 커밋 대응표](benchmarks/1.1.0-deep/evidence/commit-map.json)에
각각 같은 트리인지 확인한 결과를 남겼다. 이후 문서 변경은 검증한 제품 코드를 바꾸지 않는다.
개별 성능 비교의 기준은 별도로 적었다. 서로 다른 기준의 개선율을 합산하지 않는다.

이번 단계에서는 Thumb PUSH의 장치 쓰기 순서, IRQ 선택과 구버전 상태 복원,
지원되는 최대 SmartMedia 카드의 저장 복원을 고쳤다. 일반 RAM DMA에는 출력 동등성을
확인한 일괄 복사를 채택했다. 두 JIT 재검증 최적화는 실험 패치로 보관하며
**기본 제품 코드에는 적용하지 않았다.** 최종 네 C23 구성은 각각 49/49를 통과했고,
WASM 런타임도 통과했다. 실제 설정과 범위는 [검증 현황](#10-최종-통합-검증-현황)에 기록한다.

## 1. 무엇을 고쳤고, 무엇을 검증했는가

| 구분 | 대상 | 이번 결과 | 기본 코드 |
|---|---|---|---|
| 재현된 결함 | Thumb PUSH의 쓰기 순서와 SP 노출 | 장치 콜백 순서와 명령 완료 경계를 수정 | 포함 |
| 재현된 결함 | INTPND 복수 선택과 INTOFFSET 변경 | 선택한 IRQ를 acknowledgement까지 유지 | 포함 |
| 호환성 결함 | 실제 구버전이 저장한 복수 INTPND | 저장된 선택을 정규화해 나머지 IRQ도 처리 가능 | 포함 |
| 재현된 결함 | 128 MiB 데이터 카드의 저장 복원 거절 | spare 영역을 포함한 기존 지원 용량으로 한도 정정 | 포함 |
| 동등성 검증과 최적화 | 일반 RAM→RAM DMA | 4,160개 경우의 결과 일치, Release 비용 감소 | 포함 |
| 독립 기대값 검증 | Thumb ALU 598,800개, PC·분기 6,573,424개 | 이 범위에서 새 결함 없음 | 테스트 포함 |
| 기존 수정의 경계 검증 | CPU 코드 쓰기, 실제 I-bit 전환, 외부 쓰기 API | 기존 88개에서 90개 시나리오로 확대 | 테스트 포함 |
| 코드 정리 | 사용하지 않는 table-proof helper와 낡은 주석 | resident I-TLB 정책을 실제 구현과 일치시킴 | 포함 |
| 미채택 실험 | 감시 비트 기록의 range loop와 I=1 word 기록 생략 | 원시 측정과 패치 보관, 기본 정책 유지 | 미포함 |

테스트 개수는 결함 개수가 아니다. 예를 들어 598,800개 ALU 사례가 통과했다는 것은
그 입력과 관찰 항목에서 독립 기대값과 일치했다는 뜻이다. 모든 Thumb 명령과 예외,
실제 게임 전체가 검증됐다는 결론으로 확대하지 않는다. 출력 동등성 검사도 최종 RAM,
레지스터뿐 아니라 장치 접근 순서와 실행 중단 경계를 포함해야 한다.

이번에 더 살핀 영역은 Thumb 실행, 코드 재검증과 I/D 매핑 경계, SoC IRQ/DMA,
PWM/IIS의 실행 시점, LCD scanout 경로, 카드 저장 길이와 상태 복원이다.
기존 SSE2 오디오, 버스 쓰기 권한, CPU 자기 수정 코드의 수정은 유지하면서
새 검사와 최종 통합 실행으로 함께 확인한다. 새 물리 기기 측정이나 게임 플레이는 없다.

## 2. Thumb PUSH: 최종 RAM 검사만으로 놓치는 쓰기 순서

파일: [ARM/Thumb 코어](../src/arm920t.c),
[장치 전송 회귀 테스트](../tests/arm_thumb_transfer_test.c).
통합 커밋: `2e3522e`.

기존 PUSH는 LR부터 기록하고 낮은 레지스터들을 역순으로 기록하면서 SP를 계속 줄였다.
일반 RAM에서는 최종 주소별 값이 맞을 수 있다. 그러나 stack 주소가 관찰 가능한 장치
영역에 있으면 콜백이 받는 순서는 달라진다. 테스트 장치는 첫 두 주소를 피연산자로,
세 번째 주소를 연산 실행 명령으로 사용한다. 명령 주소에 먼저 쓰면 아직 설정하지 않은
피연산자를 사용하므로, 최종 메모리 상태가 같아도 장치 동작은 달라진다.

Samsung S3C2400 매뉴얼의 인쇄 쪽 3-41은 STM의 레지스터/주소 순서를 설명하고,
4-31은 Thumb PUSH를 `STMDB R13!`에 대응시킨다.
[ARM920T TRM](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb)의
4-12쪽도 해당 비캐시·비버퍼 영역의 STM 전송을 다룬다.
이는 임의의 정렬 방식 선택이 아니라 명령이 외부에 보이는 순서에 관한 근거다.

수정은 전체 register list로 최종 SP와 첫 주소를 먼저 계산한다. 낮은 번호의
레지스터를 낮은 주소부터 기록하고 LR을 마지막에 기록한 뒤 SP를 확정한다.
기존 ARM 전송 경로와 같이, 콜백 중에는 원래 SP가 보이고 명령의 모든 쓰기가 끝나면
최종 SP가 보인다. 주소 하위 비트와 버스 word 정렬의 구분도 유지한다.

회귀 테스트는 비어 있지 않은 모든 Thumb PUSH 목록, LR만 있는 목록, sparse 목록,
SP 하위 비트, 원본 레지스터와 flags 보존, 첫 콜백에서 stop과 IRQ를 요청하는 경우를
포함한다. stop 요청 뒤에도 그 PUSH의 남은 쓰기와 writeback을 완료하고, 다음 명령
경계에서 IRQ를 받는지 검사한다. 비교용 ARM STMDB는 raw 경로를 명시적으로 사용한다.

**총 1,030개 사례**에서 수정 전 Thumb assertion은 **6,453개 실패**했고,
ARM 대조군의 실패는 **0개**였다. 수정 후 해당 재현이 통과했다.
기존 alignment 테스트에 있던 역순 기대값도 함께 바로잡았다.
이 결과는 명령 단위 버스 콜백과 완료 경계에 대한 검증이다.
실리콘의 매 bus cycle별 SP writeback timing을 측정한 결과는 아니다.

## 3. Thumb 산술·PC 경계: 공통 구현 밖에서 기대값 계산

파일: [ALU oracle](../tests/arm_thumb_alu_test.c),
[PC·분기 oracle](../tests/arm_thumb_pc_test.c).
통합 커밋: `01377d1`, `404b145`.

이전 ARM 스트림 테스트는 raw와 cached/native 경로 사이의 차이를 찾는다.
둘이 같은 helper의 오류를 공유할 수 있으므로, 이번 Thumb 검사는 명세에 따른
기대값을 별도로 계산했다. carry와 overflow에는 넓은 정수와 signed 범위를 사용하고,
shift는 제품 코드의 0/32/큰 shift 분기를 복사하지 않은 1비트 반복 연산으로 계산한다.
PC offset도 제품의 sign-extension helper 대신 수치 범위로 구성한다.

| 테스트 | 사례 수 | 주요 범위 |
|---|---:|---|
| Thumb ALU | 598,800 | register ALU/alias, register shift의 low-byte 양 전체, immediate shift, MOV/CMP/ADD/SUB |
| Thumb PC·분기 | 6,573,424 | high-register ADD/CMP/MOV, 조건·무조건 분기, BX, ADR/literal, SP 연산, BL 두 halfword, 작은 ADD/SUB alias |

두 테스트는 16가지 초기 NZCV 조합을 포함한다. PC-relative 주소는 두 halfword
위치를 모두 살핀다. BL은 low offset 전체, signed high offset 경계와 4 KiB 경계를
포함하며 두 halfword를 별도 instruction budget으로 실행한다.
이 BL 검사는 두 halfword 사이에 IRQ를 주입하는 시나리오까지 포함하지 않는다.
예외 경계는 별도 exception 테스트의 검증 범위와 구분한다.

ARMv4T에서 UNPREDICTABLE인 형태는 정확한 숫자 하나를 기대하도록 만들지 않았다.
예를 들어 특정 MUL alias와 MUL 이후의 C flag, 일부 high-register 인코딩,
잘못 정렬된 ARM BX target을 제외하거나 명세상 정의된 부분만 검사한다.
이 범위에서는 **새 ALU·분기 결함이 발견되지 않았다.** 검사를 늘렸다는 이유로
제품 산술이나 shift 구현을 다시 쓰지 않았다.

### C23의 구현 정의 동작과 UB 구분

범위를 벗어난 unsigned→signed 변환과 음수 signed 값의 오른쪽 shift를 일괄적으로
undefined behavior라고 분류하면 안 된다. [C23 작업 초안 N3096](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf)
6.3.1.3 p3은 해당 signed 변환 결과 또는 신호를 implementation-defined로 두며,
6.5.7 p5도 음수 오른쪽 shift의 결과를 implementation-defined로 둔다.
shift 양이 타입 폭 이상인 경우 등 별도의 UB 조건과 구분해야 한다.

이번 검토는 해당 표현을 추측만으로 결함 목록에 넣거나, 검증되지 않은 signed
재작성으로 바꾸지 않았다. 독립 oracle과 실제 사용 컴파일러의 실행 결과를 남겼다.
UBSan 통과는 그 설정에서 검출한 UB가 없다는 증거이며, 모든 구현 정의 동작의
이식성이나 모든 입력에 대한 안전성을 자동으로 증명하지 않는다.

## 4. IRQ 선택 고정과 실제 구버전 저장 상태

파일: [SoC](../src/s3c2400.c), [IRQ 테스트](../tests/s3c2400_irq_test.c),
[SoC 상태 테스트](../tests/s3c2400_state_test.c).
통합 커밋: `9ecfcfb`, `8df5c41`.

### 선택된 인터럽트가 처리 중에 바뀌던 문제

기존 `check_irq`는 pending source가 달라질 때마다 우선순위를 다시 계산해
INTPND에 비트를 OR하고 INTOFFSET을 덮어썼다. TIMER1이 이미 선택된 상태에서
더 높은 우선순위의 TIMER0를 unmask하면 `INTPND=0x00000c00`, `INTOFFSET=10`이 됐다.
처리 중인 선택이 바뀌고 INTPND에는 두 비트가 남는 재현이다.

[Samsung 원본 매뉴얼](https://datasheets.chipdb.org/Samsung/S3C2400.pdf)의
14-2, 14-13, 14-15쪽은 SRCPND, arbitration 뒤의 단일 INTPND, 읽기 전용
INTOFFSET과 acknowledgement 순서를 구분한다. ISR은 source를 먼저 지우고
선택된 pending을 지운다. 두 레지스터를 단순한 pending bitmap 복사본처럼
취급하면 그 사이의 선택과 IRQ line을 보존할 수 없다.

수정 후에는 INTPND가 비었을 때만 새 IRQ를 선택한다. 선택을 유지하는 동안에는
source acknowledge나 INTMSK 변경이 발생해도 INTOFFSET과 IRQ line이 그 선택을
따른다. INTPND를 clear하면 다음 source를 선택하고, 선택이 완전히 없어지면
offset도 0으로 정리한다. INTOFFSET에 쓰는 접근은 무시한다. FIQ의 별도 경로는 유지한다.

새 IRQ 회귀는 portable/JIT 설정 모두에서 새 요청, mask 변경, 관계없는 비트의
acknowledge, SRCPND→INTPND 순서와 역순, 마지막 IRQ 해제, 저장 복원을 검사한다.
최초 수정 전 재현은 **16개 assertion 실패**였고 수정 후 통과했다.
현재의 낮은 번호 우선 선택 정책은 그대로이며, PRIORITY rotation 완성을 뜻하지 않는다.

### 옛 코어가 만든 상태를 실제로 불러본 이유

선택을 고정하는 수정만 적용하면 옛 INTPND bitmap을 그대로 유지할 수 있다.
이를 추측에 머물게 하지 않고, 변경 전 `22a4e7e`의 SoC로 두 timer 상태를 생성했다.
그 상태를 새 코어에 읽히고 저장된 INTOFFSET에 따라 실제 acknowledge를 진행했다.

| 단계 | SRCPND | INTPND | INTOFFSET | 관찰 |
|---|---|---|---:|---|
| 옛 코어가 저장 | `00000c00` | `00000c00` | 10 | 두 INTPND 비트가 직렬화됨 |
| latch 수정만 적용한 코어에서 IRQ 10 처리 | `00000800` | `00000800` | 10 | IRQ 11이 남아도 offset은 10 |
| 같은 offset을 다시 acknowledge | `00000800` | `00000800` | 10 | 처리 루프가 진전하지 않음 |
| load 정규화 적용 후 복원 | `00000c00` | `00000400` | 10 | 저장된 유효 선택을 보존 |
| IRQ 10 처리 후 | `00000800` | `00000800` | 11 | 다음 요청을 올바르게 선택 |
| IRQ 11 처리 후 | `00000000` | `00000000` | 0 | 두 요청 모두 처리 완료 |

그래서 정규화는 load 경로에만 별도 수정으로 넣었다. 저장된 offset이 범위 안이고
해당 INTPND 비트가 있으면 그 비트를 보존한다. 그렇지 않으면 실제 존재하는 비트를
선택해 offset과 일치시킨다. shift 범위를 보호하고 SRCPND는 보존하므로 다른 요청도
다음 arbitration에 참여한다. 이미 유효한 단일 선택은 유지하며 저장 형식은 늘리지 않는다.

## 5. 128 MiB SmartMedia: 데이터 용량과 저장할 NAND 길이

파일: [SmartMedia](../src/smartmedia.c),
[용량 회귀 테스트](../tests/smartmedia_capacity_test.c). 통합 커밋: `d57577b`.

에뮬레이터가 이미 지원하는 128 MiB 데이터 카드에는 각 512바이트 page마다
16바이트 spare가 붙는다. 기존 상태 입력 한도는 데이터 용량 128 MiB만 인정해서,
정상 지원 카드가 자신의 저장 상태와 compact persistent save를 다시 읽지 못했다.
mount 가능한 geometry와 복원 가능한 길이의 경계가 서로 달랐던 문제다.

| 구성 요소 | 바이트 수 | 의미 |
|---|---:|---|
| 데이터 | 134,217,728 | 128 MiB, 262,144 × 512 |
| spare | 4,194,304 | 4 MiB, 262,144 × 16 |
| NAND payload | 138,412,032 | 132 MiB, 262,144 × 528 |
| 선택적 MAME header | 1,024 | NAND payload와 별도인 입력 파일 header |
| header가 있는 최대 이미지 | 138,413,056 | 위 payload + 1,024 |

수정은 지원 데이터 한도에서 page payload 길이를 유도하고, 선택적 header의 한도를
분리한다. mount, 저장 상태, persistent save와 파일 읽기에서 같은 상한을 사용한다.
**카드/저장 상태 형식은 바꾸지 않았다.** 임의의 큰 카드 지원을 추가한 것도 아니다.
기존 128 MiB 데이터 geometry를 그 spare까지 일관되게 처리하는 수정이다.

합성 카드를 실제 공개 command/mount/save API로 조작한다. 마지막 page의 데이터와
최종 spare 바이트를 기록한 뒤 바꾸고, compact state와 persistent save에서 복원한다.
raw 이미지와 header 형식 모두 compact state와 persistent save를 검사한다.
raw 카드에서는 delta 예산을 넘는 쓰기로 기존 full-state fallback도 실행한다.
앞선 다른 block의 보존과 높은 row address를 함께 검사한다.

잘린 compact state와 지원 한도를 한 page 넘는 state는 거절해야 하며,
이 상태 복원 실패 뒤 live 카드 내용이 보존되는지도 검사한다. 별도의 oversized
이미지 mount 검사는 빈 fixture에서 거절 여부만 확인한다. 실패한 mount가 기존
카드를 보존한다는 주장이 아니다. 수정 전에는 20개 assertion이 실패했고 수정 후 통과했다.
최대 카드가 다시 로드된다는 사실을 메모리 부족 처리나 모든 손상 파일의 완전한
검증으로 확대하지 않는다.

## 6. 채택한 최적화: 일반 RAM DMA의 일괄 복사

파일: [SoC](../src/s3c2400.c), [독립 DMA oracle](../tests/s3c2400_dma_test.c),
[C 벤치마크](../scripts/bench_s3c2400_dma.c), [교차 실행 스크립트](../scripts/bench_s3c2400_dma.py).
통합 커밋: `579b325`이며, 실험 작업 트리의 같은 변경은 `ec01ca8`이다.

기존에도 **DMA2→IIS 전용 bulk 경로**가 있었다. 이번 최적화는 일반 `dma_trigger`의
RAM→RAM 전송에 추가한 별도 경로다. 오디오용 기존 경로의 성과를 이번 결과로 세지 않는다.
추가한 제품 코드는 18줄이며 새 버퍼 할당이나 저장 상태 필드는 없다.

| 빠른 경로의 조건 | 필요한 이유 |
|---|---|
| whole-service, 두 unit 이상 | 이 호출에서 전체 전송을 끝내는 기존 동작에 한정 |
| source와 destination 모두 증가 | fixed 주소의 반복 읽기/쓰기 의미 보존 |
| DSZ가 byte/halfword/word | reserved DSZ=3의 기존 처리 유지 |
| 양쪽 전체 범위가 실제 RAM 안 | BIOS/MMIO/부분 RAM의 버스 동작 배제 |
| 두 범위가 겹치지 않음 | 앞선 DMA 쓰기를 뒤의 읽기가 참조하는 순차 의미 보존 |

비중첩 RAM에서는 little-endian unit 읽기/쓰기를 byte copy로 바꿔도 값이 같다.
정렬되지 않은 주소도 포함한다. 20비트 count와 유효 DSZ가 길이를 4 MiB 미만으로
제한하고, 중첩 확인은 64비트 끝 주소 계산을 사용한다. forward overlap에 `memmove`를
적용하는 일반화는 버렸다. DMA의 앞선 쓰기가 뒤 unit의 입력이 되는 경우와 다르기 때문이다.

실제 복사 뒤에는 기존 공통 tail을 사용한다. 코드 쓰기 invalidation, current cursor,
남은 count, reload/ON 해제와 completion IRQ를 함께 유지한다. 기존 code-write 테스트의
두 word DMA도 새 경로를 통과하므로 CPU가 옛 명령을 재사용하지 않는지 검증한다.

새 oracle은 4개 channel × 4개 DSZ × 13개 배치 × 5개 count × 4개 mode,
총 **4,160개 경우**에서 모든 RAM byte와 DSTAT/DCSRC/DCDST/DMASKTRIG/SRCPND를
비교한다. 각 unit을 전부 읽은 뒤 쓰는 별도 byte 모델로 중첩과 RAM 경계를 계산한다.
**변경 전후 모두 통과**하는 동등성 검사이며, 이전 DMA에 4,160개 오류가 있었다는 뜻은 아니다.

### 기본 성능 근거: profiling을 끈 Release 180개 원시 측정

[주 측정 JSON](benchmarks/1.1.0-deep/dma-release-gcc.json)은 GCC 13.3.0 x86-64,
`-O3 -DNDEBUG -std=c2x`, `GP32EMU_CPU_PROFILE=OFF`, CPU 0 affinity 조건이다.
`CLOCK_MONOTONIC`으로 측정했고, 2026-10-17 23:51:37~23:51:55 UTC의 독립 측정
구간에는 다른 컴파일/벤치마크를 실행하지 않았다. 전후 실행 순서는 회차마다 바꿨다.

실제 CPU sink를 연결하고 공개 `s3c2400_write32`로 매번 DMA를 시작한다.
64회 warmup 후 3개 width × 6개 크기 × 전후 × 5회, 총 180개 값을 보관했다.
표본당 목표 128 MiB, 반복 횟수는 최소 256/최대 4,000,000이다.
payload와 completion register 검사는 측정 밖에서 수행하며 모든 실행이 통과했다.

153,600바이트 전송의 중앙값:

| unit 폭 | 변경 전 | 변경 후 | 해당 전송의 시간 비율(전/후) |
|---|---:|---:|---:|
| byte | 206.185 µs | 2.961 µs | 69.6× |
| halfword | 228.461 µs | 3.130 µs | 73.0× |
| word | 139.411 µs | 2.929 µs | 47.6× |

작은 대조군도 남겼다. 빠른 경로를 사용하는 8바이트는 1.19~1.53×였고,
빠른 경로에서 제외되는 단일 word/4바이트는 15.627→16.032 ns였다.
후자는 약 0.405 ns, 2.6% 증가로 관찰됐으므로 모든 크기가 빨라졌다고 쓰지 않는다.
변동성과 모든 표본은 JSON에서 확인할 수 있다.

이것은 동기식 DMA 호출 비용이다. 측정 중 guest 명령이나 SoC tick을 실행하지 않았고,
출력된 FCLK/PCLK/RUNCLK는 모두 48 MHz, speed는 100%다. 게임에서 이 경로가 쓰이는
빈도와 전체 프레임 중 비중은 측정하지 않았다. 이 비율을 게임 FPS에 적용하지 않는다.

[profiling ON 탐색 JSON](benchmarks/1.1.0-deep/dma-profile-exploratory.json)은
3회 반복, 최대 1,000,000회, 8바이트를 제외한 5개 크기의 **90개 표본**이다.
실행 순서와 측정 밖 검사도 최종 runner와 다르다. 이 자료는 보조 탐색 기록이며,
OFF의 180개 표본과 합쳐 하나의 동일 조건 표본처럼 계산하지 않는다.

## 7. 보관했지만 기본 적용하지 않은 JIT 재검증 후보

**아래 두 패치는 모두 미채택이다.** 기본 `a3e7d24`가 I=1에서 word watch 기록을
생략한다고 해석하면 안 된다. 현재 기본 코드와 실험 패치를 분리해 두었다.
원본, 적용 방법과 전체 측정 조건은 [측정 자료 설명](benchmarks/1.1.0-deep/README.md)에 모은다.

### 후보 A: 감시 비트를 범위 단위로 기록

[watch-range-loop.patch](benchmarks/1.1.0-deep/candidates/watch-range-loop.patch)는
변경되지 않은 명령을 재검증할 때 여러 word의 감시 비트 기록을 묶는 실험이다.
정확한 범위 결과를 먼저 비교했지만, 긴 재검증 이득과 짧거나 이미 뜨거운 블록의
비용이 함께 나타났다. GCC/Clang의 **profiling ON 640개 원시 표본**을 보관했다.
이 첫 실험만으로 기본 실행 경로를 교체하지 않았다.

자료: [GCC](benchmarks/1.1.0-deep/watch-range-exploratory-gcc.json),
[Clang](benchmarks/1.1.0-deep/watch-range-exploratory-clang.json).
이 자료는 다음의 더 좁은 후보 B와 코드도, profiling 설정도 다르다.

### 후보 B: I=1 동안 CPU용 word watch 기록만 생략

[skip-unused-word-watches.patch](benchmarks/1.1.0-deep/candidates/skip-unused-word-watches.patch)는
page tracking을 남기고 word tracking만 조건부로 생략하는 좁은 실험이다.
I=0 CPU store는 instruction-word watch를 사용하지만, I=1에서 컴파일한 native store는
이전 수정부터 그 guard를 생성하지 않는다. guest의 c1.I 전환은 JIT generation을
바꾸므로 I=1의 전제가 I=0 블록에 그대로 남아서는 안 된다.

외부 DMA/HLE 쓰기는 두 I 모드에서 public range invalidation을 사용한다.
그래서 후보도 **page bitmap은 항상 기록**한다. word 기록만 생략할 수 있다는 논리와
page 기록까지 생략해도 된다는 주장은 다르며, 새 테스트가 그 경계를 직접 검사한다.
기본 코드에는 해당 생략을 적용하지 않고 테스트와 재현용 driver만 남겼다.

GCC/Clang Release, profiling OFF에서 I=0/I=1을 나누고 총 **840개 표본**을 수집했다.
회차당 1,000,000개 block 실행, 조건별 전후 7회다. 48/96개 명령의 긴 native block을
I=1에서 반복 재검증하는 조건은 시간 중앙값이 약 **34~41% 감소**했다.
이는 dispatch와 해당 guest block 실행을 포함한 합성 비용이다.

| I=1 native 조건 | GCC 전→후(ns/block) | Clang 전→후(ns/block) |
|---|---:|---:|
| 48개 명령, working set 64, 매번 재검증 | 111.263→73.417 | 111.437→73.898 |
| 96개 명령, working set 64, 매번 재검증 | 189.235→115.823 | 192.126→116.587 |
| 2개 명령, working set 1, maintenance 없음 | 11.259→11.282 | 11.307→11.748 |

짧은 hot 대조군과 I=0 조건에는 작은 개선과 악화가 섞였다. 실제 게임에서 긴 재검증이
얼마나 자주 일어나는지, ARM 기기에서는 어떤 비용이 드는지 확보하지 못했다.
따라서 논리적으로 타당하고 특정 합성 조건에서 빠른 후보를 **기본 성능 개선으로
승격하지 않았다.** 사용자는 나중에 같은 게임 상태와 기기에서 선택적으로 비교할 수 있다.

자료: [GCC I=1](benchmarks/1.1.0-deep/watch-skip-release-gcc-i1.json),
[GCC I=0](benchmarks/1.1.0-deep/watch-skip-release-gcc-i0.json),
[Clang I=1](benchmarks/1.1.0-deep/watch-skip-release-clang-i1.json),
[Clang I=0](benchmarks/1.1.0-deep/watch-skip-release-clang-i0.json).
profiling OFF에서는 native 명령 counter가 0일 수 있다. driver는 준비된 native block,
retired 명령, 결과와 generation/생성 코드 크기를 검사하며 ON 설정의 counter와 구분한다.

### 기본 테스트에 남긴 것과 optional patch에만 있는 것

[CPU code-write 테스트](../tests/arm_cpu_code_write_test.c)는 기본 제품에서 **90개**다.
I=1 상태의 writer와 target을 모두 준비한 뒤 실제 guest MCR로 I=0으로 바꾸어,
옛 블록의 전제가 취소되는지 더 강하게 검사한다. I=1을 유지한 채 외부 API로
코드 쓰기를 알렸을 때도 새 명령을 보아야 한다는 두 실행 모드의 사례를 추가했다.

후보 B patch의 **954개 metadata oracle은 기본 49개 CTest에 포함되지 않는다.**
후보를 적용한 격리 x86-64 검사에서는 CPU 90개와 metadata 954개가 통과했다.
AArch64/QEMU 격리 검사도 CPU 90개와 metadata 954/954가 통과했다(seed `cd3018d4`).
return 위치를 잘못 옮긴 변형은 697/954만 통과했고 I=1에서 257개가 실패해 exit 1이었다.
**CPU 90개만으로는 이 변형을 검출하지 못했다.** 잘못된 metadata 생략을 검출한 근거는
추가 oracle이다. 기본 코어, optional patch, 의도적으로 틀린 변형의 결과를 구분한다.

## 8. 이전 최적화와 거절 기록 재검토

기존 쓰기 권한 분리, I=0 자기 수정 코드, SSE2 FIR은 새 변경과 함께 검증 대상에 둔다.
SSE2는 x86 경로의 측정 결과이며 ARM NEON의 성과로 옮겨 적지 않는다.
256 KiB급 word 감시표의 메모리 비용과 I=0 일반 store 비용도 이전 보고서에 남아 있다.
이번 검사 확대나 미채택 JIT 후보가 그 비용을 없앴다고 주장하지 않는다.

사용하지 않는 table mapping helper 두 개를 제거하고, 재검증이 resident I-TLB만
사용한다는 주석으로 정리했다(`14c7420`). 현재 page table을 다시 걷는 새 최적화를
도입한 것이 아니다. 이는 실제 코드와 설명의 불일치를 줄이는 정리다.

과거 성능 판단은 `d93704f7`의
[GP32_PERFORMANCE_STRATEGY.md](https://github.com/gajae1/gp32emu/blob/d93704f7/docs/GP32_PERFORMANCE_STRATEGY.md)
내용을 읽어 검토했다. 아래 숫자는 **그 문서에 기록된 과거 기기 결과**다.
당시 `F:/GP32/results/` 원시 자료는 이번 환경에 없고, 관련 historical branch 객체도
확보되지 않아 벤치마크 재실행이나 patch-id 동일성 검증을 했다고 주장하지 않는다.

| 과거 후보 | 문서에 기록된 관찰 | 이번 검토의 해석 |
|---|---|---|
| framed leaf-callee splice `80bb31d` | A53 1,512 MHz에서 LGM boot +1.88%, Her Knights 185.369→102.897 fps, 약 -44.5%; 출력은 동일 | boot 한 구간의 이득으로 call-heavy 경로의 큰 회귀를 덮을 수 없음 |
| packed AArch64 RAM-page/TLB lookup | Princess -2.57%, Astonishia +2.08%, Blue +3.75%; 코드 크기는 감소 | 명령 수·코드 크기 감소만으로 채택하지 않았던 결정을 존중 |
| adjacent ALU operand의 LDP | Her Knights +0.86%, Tomak +0.78%, Wizard -0.58% | 작은 혼재 결과로 기록된 미채택 후보 |
| poll mapping-result 재사용 | Princess 77.895→77.5155 core fps, -0.49%, 출력 동일 | 동일성을 확인해도 전체 비용의 유용한 감소가 따로 필요 |

이 기록은 이번 두 JIT 후보를 보류한 이유와 연결된다. 코드를 덜 생성하거나 같은
계산을 한 번 줄이는 것만으로 약한 기기에서 이득이 보장되지는 않는다.
profiling ON/OFF와 boot/gameplay 구간을 분리해야 한다는 과거 경험도 반영해,
DMA의 주 근거와 후보 B는 정상 Release에서 다시 측정했다.
과거 여러 장면의 개선율을 이번 결과에 곱하거나 합산하지 않는다.

## 9. 하드웨어 자료를 어디까지 확보했는가

[Samsung S3C2400 원본 매뉴얼](https://datasheets.chipdb.org/Samsung/S3C2400.pdf)을
확보해 ARM/Thumb 전송, interrupt controller와 timer 설명의 근거로 사용했다.
SoC 내부 block diagram은 CPU와 DMA/IIS/LCD/interrupt controller의 관계를 설명한다.
이는 **GP32 완성 보드의 회로도나 netlist가 아니다.**

이전 검토의 [Hitmen 원래 GP32 소스 페이지](https://hitmen.c02.at/html/gp32_sources.html)와
[PCM 예제 v0.02](https://hitmen.c02.at/files/releases/gp32/gp32_sound_v0.02.zip)는
DMA2/IIS와 GPIO L3 제어를 보여 주는 원본 예제다.
[NXP UDA1330ATS](https://www.nxp.com/docs/en/data-sheet/UDA1330ATS.pdf)의 17쪽 Fig. 9는
DAC 응용 회로다. 두 자료와 Samsung block diagram을 조합해 GP32 보드 배선을
검증했다고 말할 수 없다. GP32 전체 board schematic과 리비전별 실물 확인은 여전히 미완료다.

ARM920T의 ISA는 ARMv4T이며, DAC가 있다는 사실이 ARMv5TE DSP 곱셈 명령을
추가하지 않는다. 이 구분의 근거와 원래 예제의 빌드 타깃은 이전 추가 보고서에 있다.
이번에 별도 오디오 DSP를 발견하거나 보드 모델을 새로 추가한 것은 아니다.

## 10. 최종 통합 검증 현황

대상 기본 코드는 `a3e7d24`, 네 CMake 구성 모두 `GP32EMU_REQUIRE_C23=ON`,
tests/libretro/profiling ON이며 각 CTest를 `-j2`로 실행한다. 기본 대상은 49개다.
DMA 주 성능 비교의 profiling OFF 설정과 이 기능 검증 설정은 서로 다르다.

| 구성 | dialect / 핵심 설정 | 빌드 | 최종 CTest |
|---|---|---|---|
| GCC 13.3 x86-64 Release | `-O3 -DNDEBUG -std=c2x` | 통과 | 49/49, 7.59초 |
| Clang 18.1.3 x86-64 Release | `-O3 -DNDEBUG -std=c23` | 통과 | 49/49, 6.88초 |
| GCC 13.3 AArch64 Release + QEMU | `-std=c2x`, `qemu-aarch64 -L /usr/aarch64-linux-gnu` | 통과 | 49/49, 60.31초 |
| GCC 13.3 x86-64 RelWithDebInfo + UBSan | `-O2 -g -DNDEBUG`, UBSan 복구 금지 | 통과 | 49/49, 8.81초 |
| Clang/LLD 18 WASM + Node | Makefile.wasm, C23 강제 | 통과 | WASM 런타임 검증 통과 |
| optional 후보 B AArch64, 격리 검사 | 기본 제품 및 위 49개와 별개 | 통과 | CPU 90개 + metadata 954/954 |

WASM은 실제 sine/filter gain, monotonic clock, VFS 성장·실패·clear 소유권을 검사했다.
9,290,562바이트 상태를 32회 저장했으며 준비 후 heap-used는 **38,340,656바이트**로
유지됐다. 이 값은 allocator의 heap 사용 위치 지표로, 브라우저 RSS나 linear memory
크기가 아니다. 실제 브라우저 렌더링·오디오 출력과 물리 기기 검증은 포함하지 않는다.

UBSan flags는 `-fsanitize=undefined -fno-sanitize-recover=all
-fno-omit-frame-pointer`다. GCC의 `__STDC_VERSION__`은 `202000L`, Clang은
`202311L`로 기록한다. CMake가 C23 dialect를 선택했다는 사실을 컴파일러의
최종 ISO C23 기능 전체 구현이나 자동 성능 향상으로 해석하지 않는다.
QEMU 결과는 AArch64 경로의 기능 검증이며 물리 ARM 성능 측정이 아니다.
위 CTest 실행 시간도 성능 비교값이 아니라 검증 실행 기록이다.

기본 재현 명령은 다음과 같다. compiler/runner를 바꿀 때도 별도 build 디렉터리를 쓴다.

```bash
cmake -S . -B build-deep -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGP32EMU_REQUIRE_C23=ON -DGP32EMU_BUILD_TESTS=ON \
  -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_CPU_PROFILE=ON
cmake --build build-deep -j2
ctest --test-dir build-deep --output-on-failure -j2

make -f Makefile.wasm build test GP32EMU_REQUIRE_C23=1 \
  WASM_CC=clang-18 WASM_LD=wasm-ld-18 -j2
```

## 11. 남은 정확성·기기 검증 과제

| 항목 | 현재 한계와 다음에 필요한 증거 |
|---|---|
| WFI | 이전 fall-through 재현이 남음. wait 상태, IRQ wake, SoC 시간과 상태 복원을 함께 설계해야 함 |
| FCSE PID | nonzero PID의 modified-address 처리가 미완료. walker뿐 아니라 tags/proof/전환 경계 필요 |
| 전체 I-cache 모델 | I=1 noncacheable mapping과 실제 cache/pipeline 정책까지 완성한 모델은 아님 |
| AP/domain fault | host fastmem 쓰기 권한 수정과 별개인 ARM MMU fault 정책의 미완료 영역 |
| IRQ priority rotation | 이번 latch 수정은 선택 유지에 한정. PRIORITY/회전 arbitration 완성은 후속 작업 |
| PWM double buffering·정지 | TCNTB의 live 사용과 counter 보존 경계를 코드/매뉴얼에서 확인했으며 별도 재현과 상태 설계 필요 |
| 물리 기기 | Android/iOS/A53급 handheld의 실제 게임, frame pacing, 음성 출력, 발열·전력 측정 미완료 |

저사양 기기의 다음 판단에는 동일한 저장 장면과 입력으로 guest 상태·영상·PCM을
검증한 뒤, 정상 Release에서 시간 분포를 측정하는 작업이 필요하다.
DMA 사용량, 재검증 빈도와 길이, ARM/Thumb 비율, compile 시간과 RSS를 함께 보면
어느 후보가 그 장면에 필요한지 판단할 수 있다. CPU 주파수·온도·frontend 조건도 기록한다.
이번 보고서의 큰 합성 개선율만으로 에뮬레이트하는 GP32 clock을 올리거나,
JIT cache 크기를 일괄 줄이거나, optional patch를 기본 적용하지 않는다.

## 12. 근거와 재현 자료

- [Samsung S3C2400 매뉴얼](https://datasheets.chipdb.org/Samsung/S3C2400.pdf): STM 3-41, PUSH 4-31, PWM 10-4~10-5, IRQ 14-2/14-13/14-15.
- [ARM920T TRM DDI 0151C](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb): bus/cache, I-cache disable, WFI와 FCSE.
- [ARM ARM DDI 0100I](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a): A7 Thumb 연산·분기와 UNPREDICTABLE 조건.
- [C23 N3096](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n3096.pdf): 6.3.1.3 p3, 6.5.7 p5.
- [이번 원시 측정·실험 패치 설명](benchmarks/1.1.0-deep/README.md), [이전 추가 측정 설명](benchmarks/1.1.0-followup/README.md).
- [변경 전 실패와 최종 검증 로그](benchmarks/1.1.0-deep/evidence/README.md), [소스 해시와 검증 설정](benchmarks/1.1.0-deep/evidence/validation.json).
- [이전 CPU 코드 쓰기 분석](CPU-CODE-WRITES.md), [이전 전체 검토](REVIEW-1.1.0.md), [이전 한국어 추가 검토](REVIEW-1.1.0-FOLLOWUP.ko.md).

하드웨어 문서, 독립 기대값, 변경 전 재현, 최종 통합 검사, 합성 호스트 시간과
과거 기기 기록은 각자의 범위를 가진다. 해당 범위를 유지해야 이후의 기능 수정과
성능 실험에서도 이번 결과를 신뢰할 수 있는 기준으로 사용할 수 있다.
