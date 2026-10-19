# Round-four functional validation evidence

The final GCC, Clang, AArch64/QEMU and GCC/UBSan matrices each pass **54/54
CTest tests**. This directory preserves their complete output, build settings,
source identities and earlier unsuccessful integration attempts. The machine-readable
index is [validation.json](validation.json); the findings and implementation
decisions are in the [main review](../../../REVIEW-1.1.0-ROUND4.ko.md).

CTest wall times describe these functional runs. They are not emulator speed
measurements. The separate [benchmark index](../README.md) records the controlled
performance experiments and their source variants.

## Source identity

| Role | Local verification commit | Public commit | Git tree |
|---|---|---|---|
| Round-four entry baseline | `b141220cd5fd9d628305133fc963ed86a30f7e41` | [b141220](https://github.com/gajae1/gp32emu/commit/b141220cd5fd9d628305133fc963ed86a30f7e41) | `4aa7e7db55a749eadad7a4a5f423f62de32afb68` |
| Production source freeze | `06c4d879543f90d1003d174718d738e27a8ed6b8` | [aa54dfc](https://github.com/gajae1/gp32emu/commit/aa54dfc0f2b63b7d81e8c68be249ecc7431304d6) | `1fd43d466c20f9dd3e470a6a1f065f3ca283f70e` |
| Final test-source freeze | `eebb337b7133fa19b330fe82fd9f002fa215b8b4` | [af387ea](https://github.com/gajae1/gp32emu/commit/af387eaeb0687b250e8815a1795cb88b709195ee) | `dd3b0d94445c3053d95faba2d59fd71fd7d9df65` |

The [commit map](commit-map.json) records the public/local correspondence and
tree equality. Raw logs and metadata retain their original local commit IDs and
temporary paths.

The production-to-test-freeze delta contains only
`tests/arm_exception_test.c` and `tests/arm_poll_test.c`. The `src` trees are
identical. Subsequent documentation commits do not change these production
bytes. All three root runners recorded the same **183 source-file SHA-256
values**; those values were also checked against the frozen Git objects while
assembling this evidence.

These equality checks concern the recorded build inputs and source/test bytes.
They do not require documentation or CI invocation files added or corrected
after the matrices to match the test-source freeze. Any later CI invocation
check has its own evidence and does not change the provenance of these runs.

The UBSan runner started at the same test-source freeze. Its completion HEAD
advanced through a documentation-only commit; the runner recorded both HEADs
and verified that the complete non-documentation source diff was empty. The
statement describes that recorded completion time, not every later HEAD. The
original record is in [UBSan metadata](final-ubsan/metadata.json).

## Final integration matrices

| Configuration | Actual C dialect and optimization | Result | CTest wall time | Complete output and metadata |
|---|---|---|---|---|
| GCC 13.3.0, x86-64, fresh Release build | `-std=c2x -O3 -DNDEBUG` | 54/54 | 7.56 s | [LastTest](final-gcc/LastTest.log), [CTest](final-gcc/ctest.log), [metadata](final-gcc/metadata.json) |
| Clang 18.1.3, x86-64, Release | `-std=c23 -O3 -DNDEBUG` | 54/54 | 7.53 s | [LastTest](final-clang/LastTest.log), [CTest](final-clang/ctest.log), [metadata](final-clang/metadata.json) |
| GCC 13.3.0, AArch64, Release under QEMU | `-std=c2x -O3 -DNDEBUG` | 54/54 | 66.69 s | [LastTest](final-aarch64/LastTest.log), [CTest](final-aarch64/ctest.log), [metadata](final-aarch64/metadata.json) |
| GCC 13.3.0, x86-64, fresh RelWithDebInfo + UBSan | `-std=c2x -O2 -g -DNDEBUG` plus sanitizer flags | 54/54 | 8.84 s | [LastTest](final-ubsan/LastTest.log), [CTest](final-ubsan/ctest.log), [metadata](final-ubsan/metadata.json) |

Every configuration has `GP32EMU_REQUIRE_C23`, `GP32EMU_BUILD_TESTS`,
`GP32EMU_BUILD_LIBRETRO`, `GP32EMU_BUILD_HEADLESS` and `GP32EMU_CPU_PROFILE` ON,
with `GP32EMU_ENABLE_IPO` OFF. The cache values, all **72 compilation commands**,
the 54-test inventories and CMake's generated compiler identities were checked.
The core and full-JIT-test commands contain `-DGP32EMU_CPU_PROFILE=1`; no
compilation command contains `-flto`. GCC's `c2x` spelling and Clang's `c23`
spelling are recorded separately. This is verification of the project's requested
C23 mode, not certification of every ISO C23 language and library feature.

Build and CTest parallelism were both `-j2`. The AArch64 inventory invokes
`qemu-aarch64 -L /usr/aarch64-linux-gnu` for the test executables. This executes
the A64 generated-code path under emulation; it supplies no physical-AArch64
performance measurement.

For UBSan, all compilation commands and the executable/shared linker settings
include `-fsanitize=undefined -fno-sanitize-recover=undefined`. The runtime used
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Its complete LastTest log
contains **zero UBSan runtime diagnostics**.

Each `final-*` directory contains `configure.log`, `build.log`, `ctest.log`,
`LastTest.log`, `metadata.json`, `CMakeCache.txt`, `CMakeCCompiler.cmake`,
`compile_commands.json`, `test-inventory.json` and a warning classification.
The GCC/Clang/AArch64 `metadata.json` files are unchanged copies of the root
runner's original `validation.json` files. The UBSan directory additionally
retains its original diagnostics and runner. [validation.json](validation.json)
records the original locations and SHA-256 values of all 38 copied artifacts.
The copied LastTest files match CTest's original files byte for byte.

Raw CMake/CTest files keep the generators' original trailing whitespace and
final blank lines; exported reconstruction patches keep their context-line
spaces. Those archived bytes are intentional and hash-checked. Whitespace
checks on authored source and prose are separate from preserving raw evidence.

## JIT execution evidence and counter units

The full `arm_jit` test prints the following summaries:

| Configuration | JIT events | Reported fallbacks |
|---|---:|---:|
| GCC x86-64 | 716,432 | 0 |
| Clang x86-64 | 716,432 | 0 |
| AArch64/QEMU | 115,386 | 0 |
| GCC x86-64 + UBSan | 716,432 | 0 |

The unit is explicit in `teardown_pair()` in
[arm_jit_test.c](../../../../tests/arm_jit_test.c): the harness adds
`arm920t_get_jit_hits()` and `arm920t_get_jit_misses()` across its CPU instances.
These are cache/dispatch events, **not retired native instruction totals**.
The logs do not print aggregate native ARM and Thumb retirement totals, and the
event totals must not be converted into such totals or treated as a speed ratio.

Actual native execution is checked by the retained positive profiling assertions
inside the full JIT and complete-BIOS-window tests. The detailed logs also print
individual native controls, including conditional execution and the flush/framed
loops. Individual controls are not summed into an invented aggregate. Tests
that intentionally decline native execution or acceleration retain their refusal
checks.

The separate Thumb oracles print the same zero-failure results in all four
matrices:

| Oracle | Printed coverage | Failures |
|---|---:|---:|
| Thumb ALU | 598,800 cases | 0 |
| Thumb PC/control flow | 6,573,424 instruction cases | 0 |
| Thumb register transfers | 14,278 cases | 0 |

These are oracle case counts. They are not counts of bugs, JIT cache events or
retired native instructions. The separate seeded ALU differential's printed
native-instruction counter is likewise not the full `arm_jit` total.

## Switch workflow invocation repair

The manual Switch workflow passed `src/arm920t.c` as a separate compiler input
even though `tests/arm_jit_test.c` includes it to inspect native internals.
The original invocation reproduced 45 multiple-definition diagnostics and link
exit 1. Removing only the redundant argument produced a successful AArch64
build with the same flags. The full differential then passed in both the
CodeMemory and SetProcessMemoryPermission mocks: **115,386 JIT events and zero
fallbacks per mode**. Source, tests and the mock implementation did not change.

The separate [Switch CI record](switch-ci/README.md) retains the exact commands,
logs, compiler identity and source/workflow hashes. These local QEMU runs are
not a devkitPro/libnx SDK or NRO build, a physical Switch measurement, or a remote
workflow run. This CI invocation repair followed the four matrices and does not
replace or alter their source provenance.

## Earlier integration attempts and the fixture repair

The first integration runs at production freeze `06c4d87` passed **52/54** tests
on Clang, AArch64 and UBSan. `arm_poll` and `arm_exception` exposed old fixture
assumptions after the bus-contract changes. Their native controls allocated only
64 KiB of BIOS backing, while the native backend now correctly requires the
complete 512 KiB window. The literal and page-table callback arrangements also
overlapped the complete direct BIOS mapping.

The test-only repair made three bounded setup changes:

- Allocate the full `0x80000` BIOS backing in both fixtures.
- Keep only the literal data word callback-backed, while preserving direct
  instruction fetch in the literal polling case.
- Use separate 16 KiB table backing at aligned physical address `0x12000000`,
  outside the complete BIOS and RAM windows, with a full-span bounds check and
  the existing no-fastmem table behavior. Existing whole-bus initialization
  clears the new backing.

All native and architectural assertions remain. The fully callback-backed
[fetch regression](../../../../tests/arm_fetch_callback_test.c) remains intact
and passes. No production source changed in this fixture repair.

In the focused UBSan reproduction, `arm_poll` went from **35 assertion
failures to 0**, and `arm_exception` went from **984 to 0**. The intermediate
BIOS/literal repair left three MMU native-coverage assertions failing; moving
the callback-backed page table resolved those remaining fixture conflicts.
The final focused [build](fixture-repair-build.log),
[CTest](fixture-repair-ctest.log) and [full output](fixture-repair-LastTest.log)
are retained. The four final full matrices above include the repair.

The first GCC integration attempt is a different issue. Changing an existing
CMake compiler path from `cc` to `gcc` caused cache regeneration and reset
options. Old CTest files still ran and printed **49/49**, despite the new suite
requiring 54 tests. That result was **rejected as final validation**, even though
CTest returned success. The [original configuration warning](integration-first-attempt/gcc-configure.log)
and [explicit rejection metadata](integration-first-attempt/gcc-validation.json)
are preserved. A fresh GCC build with checked cache values and a 54-test
inventory supplies the accepted result.

The [initial-attempt directory](integration-first-attempt/) preserves the failed
52/54 runs, rejected GCC result and original UBSan diagnostics separately from
the final evidence. Neither the failures nor the rejected result were overwritten
with a later passing log.

## Baseline scope and red-to-green regressions

[baseline-gcc-ctest.log](baseline-gcc-ctest.log) is the legitimate **historical
49/49** run at the round-four entry baseline, `b141220`. It is distinct from the
rejected stale 49-test integration run above. The baseline had the earlier test
inventory; it did not include all later regressions.

Most rows below are development **per-fix pre-change harnesses**, paired with
the corresponding repaired implementation. They are not claims that the entire
final 54-test tree was run unchanged at `b141220`. The callback-fetch row is
explicitly different: its final oracle was replayed against the exact
`arm920t.c` from public baseline `b141220`. Performance source variants have
their own hashes and [reconstruction patches](../isolation/README.md); they are
also distinct from the final integration freeze.

| Regression | Scenario or input coverage | Failed assertions before → after | Recorded logs |
|---|---|---:|---|
| ARMv4T control allocation | 2,700 allocation/operand/engine/phase cases | 4,874 → 0 | [before](arm-control-before.log), [after](arm-control-after.log) |
| Complete BIOS read window | 10 mapping configurations | 220 → 0 | [before](bios-window-before.log), [after](bios-window-after.log) |
| Callback-backed opcode fetch | 162 scenarios; final oracle against exact baseline `arm920t.c` | 4,281 → 0 | [baseline replay](fetch-callback-before.log), [after](fetch-callback-after.log) |
| HLE RAM zero and code coherence | 71 cases | 4 → 0 | [before](hle-zero-before.log), [after](hle-zero-after.log) |
| HLE EEPROM/ID code coherence | 12 engine/I-bit/output scenarios | 8 → 0 | [before](eeprom-code-before.log), [after](eeprom-code-after.log) |
| HLE application-query output range | 39 address/engine scenarios | 18 → 0 | [before](app-query-before.log), [after](app-query-after.log) |
| DMA software trigger | 72 trigger scenarios | 324 → 0 | [before](dma-trigger-before.log), [after](dma-trigger-after.log) |
| IRQ priority and rotation | 2,508 arbiter scenarios | 10,042 → 0 | [before](irq-priority-before.log), [after](irq-priority-after.log) |
| Saved framebuffer dimensions | Six invalid tuples plus three valid historical controls | 12 → 0 | [before](framebuffer-state-before.log), [after](framebuffer-state-after.log) |
| Save/rejected-load clock phases | 52 scenarios; 20 affected before repair | 114 → 0 | [before](state-speed-before.log), [after](state-speed-after.log) |

Scenario counts and failed assertion counts are different units. Some oracles
limit the number of printed failure details while retaining a complete failure
counter; counting visible `FAIL` lines would therefore understate the recorded
total. These totals agree with the [main review](../../../REVIEW-1.1.0-ROUND4.ko.md).

The clock-phase regression verifies exact restoration of live PWM, IIS, idle
audio and LCD progress for saves and rejected loads. Normal speed restoration
can legitimately invalidate derived caches. The fixture deliberately starts
with the serialized IIS cache-dirty flag set before its const snapshot, so its
complete serialized comparisons do not mistake legitimate cache invalidation
for phase drift. Successful loads preserve incoming phases with normal host-speed
conversion. This evidence does not claim that every internal cache byte remains
unchanged.

## Retained controls and unresolved probes

The DMA log also retains the earlier **4,160-case RAM oracle**, with zero
failures before and after the software-trigger fix. This is continuing coverage
of the previously accepted RAM bulk-copy path, not a new round-four optimization.
The isolated [GCC](thumb-transfers-gcc.log) and
[Clang](thumb-transfers-clang.log) transfer logs each show **14,278 passing
cases** for the Thumb optimization.

The [optional A64 second-way chaining evidence](optional-a64-chain/README-optional-a64-chain-06c4d879.md)
contains a separate 30-scenario focused run and full-JIT check for an optional
patch. That patch is excluded from the default production freeze and the four
final matrices here. Its functional evidence is not physical-device timing.

The [PWM probe](pwm-model-gap.c) and [25 model mismatches](pwm-model-gap.log)
remain explicitly unresolved. This diagnostic is not a passing test in the
54-test inventory or a claimed fix. It documents the live-counter/buffer/stop
model gap and the need for a contained timer/state design before changing that
behavior.

## WASM runtime control

[wasm-runtime.log](wasm-runtime.log) preserves the C23 `clang-18`/
`wasm-ld-18` build and Node execution. Its seven printed passing checks cover
constant PCM/filter gain, the monotonic host clock, VFS growth, close/clear
ownership, allocation failure and repeated state saving. Repeated
**9,290,562-byte** saves reuse a **38,340,656-byte** heap. These values describe
the existing runtime regression; they are not a newly measured WASM optimization.

The WASM production source is the same `src` content as the production freeze.
The later two-fixture repair and documentation changes do not alter those bytes.
The captured WASM build has one unused-`itlb_store` warning at `src/arm920t.c`;
its originating commit also predates `b141220`. The exact diagnostic is indexed
in [validation.json](validation.json).

## Compiler warnings and sanitizer scope

| Captured final build log | Compilation steps in that log | Warning emissions | Normalized sites | Classification |
|---|---:|---:|---:|---|
| [GCC Release](final-gcc/build.log) | 72, fresh full build | 96 | 14 | [Full classification](final-gcc/diagnostics.json) |
| [Clang Release](final-clang/build.log) | 2, repaired fixtures only | 0 | 0 | [Log scope](final-clang/diagnostics.json) |
| [AArch64 Release](final-aarch64/build.log) | 2, repaired fixtures only | 0 | 0 | [Log scope](final-aarch64/diagnostics.json) |
| [GCC UBSan](final-ubsan/build.log) | 72, fresh full build | 40 | 13 | [Full classification](final-ubsan/diagnostics.json), [original UBSan audit](final-ubsan/ubsan-diagnostics.json) |

Clang and AArch64 reused the already-built, source-hash-checked production
objects and rebuilt the two changed fixtures. Their zero-warning final logs
**do not establish warning-free clean builds**. The complete 54-test execution
was repeated for both configurations.

The GCC/UBSan union contains **15 source sites**. Each site's Git blame origin
was checked and is an ancestor of baseline `b141220`. The diagnostics include
the exact location, message, repeated emission count and originating commit.

| Diagnostic category | GCC emissions / sites | UBSan emissions / sites |
|---|---:|---:|
| `-Wtype-limits` | 11 / 4 | 11 / 4 |
| `-Wunused-function` | 9 / 4 | 9 / 4 |
| `-Wmaybe-uninitialized` | 74 / 4 | 17 / 2 |
| `-Wformat-truncation` | 2 / 2 | 3 / 3 |

These are retained diagnostics from existing source sites, not claims that the
same warning counts appeared in every historical build. Optimization level,
compiler and rebuild scope affect emission counts. No unrelated product edits
were made to suppress them. The original UBSan matrix and the final UBSan
matrix have matching warning messages, sites and occurrence counts; both have
zero runtime UBSan diagnostics.

This package validates the specified synthetic core, bridge and runtime tests.
It does not add Android/iOS SDK application runs, physical handheld results or
game/BIOS-corpus coverage beyond the scope recorded in the main review.
