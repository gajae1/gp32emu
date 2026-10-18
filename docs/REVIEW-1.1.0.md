# GP32emu 1.1.0 core and platform review

Reviewed on 2026-10-17 against main commit
[`1090ff174459c57d42106a028cf4e16b2f5fe618`](https://github.com/gajae1/gp32emu/commit/1090ff174459c57d42106a028cf4e16b2f5fe618).
The accompanying changes correct five bounded issues and strengthen a native
execution regression fixture. The runtime memory improvement is demonstrated
with repeated synthetic WASM saves. Gameplay speed, power consumption, and
device compatibility still require measurements on the intended hardware.

The [Korean follow-up report](REVIEW-1.1.0-FOLLOWUP.ko.md) adds the GP32 audio
hardware/ISA investigation, further core fixes, exact SSE2 FIR measurements,
rejected experiments, and the expanded C23 validation. The subsequent
[Korean deep review](REVIEW-1.1.0-DEEP.ko.md) adds Thumb bus/IRQ/card fixes,
guarded RAM DMA, independent Thumb oracles, optional JIT experiments and the
final 49-test matrix. Counts below describe the original review snapshot;
see the [fourth Korean review](REVIEW-1.1.0-ROUND4.ko.md) for the latest
JIT/interpreter changes, peripheral and state fixes, and integrated results.

## Review scope

The review followed the ARM/Thumb interpreter, shared decoded blocks, x86-64 and
AArch64 emitters, translation and invalidation, SoC LCD/DMA/timer/IRQ paths,
loading and saved states, shared audio, and relevant frontend boundaries.
Platform inspection covered libretro, SDL, Android, the iOS bridge, Qt, and the
WASM runtime/web frontend. Native core tests ran on Linux x86-64 and on Linux
AArch64 under QEMU; WASM modules ran in Node's WebAssembly engine. Android/iOS
SDK builds, physical handhelds, browser rendering/audio output, and real games
were outside the executed validation.

The existing implementation already has substantial optimizations: compact
20-byte decoded operations, two-way block lookup, lazy native compilation,
short-leaf and framed-call tracing, AArch64 register caching and block chaining,
fetch revalidation proofs, separate instruction/data translation caches,
guarded polling shortcuts, and reusable audio coefficients. The next useful
performance change depends on which path dominates a slow title's profile.

## Changes included

### 1. Preserve live LR when a general native trace returns

Files: [arm920t.c](../src/arm920t.c),
[AArch64 emitter](../src/arm920t_jit_a64.inc),
[JIT regression tests](../tests/arm_jit_test.c).

The translator used return marker 7 both for register-only ALU leaves and for
general traced callees containing memory accesses. Both native emitters treated
that marker as proof that a return could be omitted. A bus callback can change
LR while leaving the helper's checked PC, CPSR, generation, epoch, and deadline
unchanged. A subsequent `MOV pc,lr` or `BX lr` then followed the recorded caller
instead of the live return address. An odd replacement LR also failed to enter
Thumb state.

`ARM_JIT_RETURN_GUARDED` and `ARM_JIT_RETURN_LEAF` now express the two different
proofs. General returns use their existing dynamic checks; the register-only
collector retains return elision. Final-operation cleanup and prefix
classification recognize both markers. There is no new CPU state or saved-state
format field.

The new regression changes LR during an MMIO read, covers MOV and BX returns
including a Thumb target, and compares budgets 1, 2, 3, 4, and 32 with a raw
interpreter reference. It produced 13 state mismatches on each native backend
before the fix and passes on both after the fix. The full-budget case asserts
that native instructions actually executed.

### 2. Reject ARMv5TE signed DSP multiply encodings on ARM920T

Files: [shared validator](../src/arm920t.c),
[exception tests](../tests/arm_exception_test.c).

The signed DSP multiply families `SMLAxy`, `SMLAWy`, `SMULWy`, `SMLALxy`, and
`SMULxy` previously reached PSR decoding and silently did nothing. They are
ARMv5TE extension instructions, unavailable on ARM920T. One mask in the shared
ARMv4T validator now sends them through the existing undefined-instruction path:

```c
(insn & 0x0f900090u) == 0x01000080u
```

The condition still determines whether the instruction executes. Tests cover
all 16 halfword variants, failed and passing EQ conditions on the same cached
block, exception vector/CPSR/banked LR, unchanged operands, and exception return.
Six ordinary ARMv4 multiply instructions are positive controls. Profiling
assertions measure the two-instruction source block before its handler runs, so
native activity in the handler cannot disguise missing native coverage.

The new tests produced 384 expected exception assertions against the old core
on each of x86-64 and AArch64, with coverage checks and valid multiply controls
passing. Fixed tests also pass with the native JIT disabled at compile time.
Encoding references: ARM DDI 0100I, A3.16.3 and A4.1.74/77/79/86/88; ARM920T
undefined-instruction behavior: DDI 0151C, 2.2.2.

### 3. Use real sine for the WASM audio filter

Files: [freestanding libc](../wasm/libc/src/pcfx_wasm_libc.c),
[web imports](../web/gp32-wasm.js),
[WASM C fixture](../tests/wasm_runtime_test.c),
[WASM runner](../tests/wasm_runtime_test.mjs).

The WASM libc implemented `sin(x)` as `return x`. The shared resampler uses sine
to build its sinc kernel. In an actual WASM execution, constant PCM of 10,000
became 32,767; coefficient rows summed to 950,272 instead of 32,768. The native
resampler preserved the same input.

The WASM implementation now imports `pcfx.sin`, supplied by `Math.sin`. The
shared filter is unchanged. Tests check every output sample for positive and
negative constant input, and every coefficient row, at 11,025→44,100,
23,144→44,100, 48,000→44,100, and 23,144→48,000 Hz. All pass.

Publish the updated JavaScript and WASM together: the audio fix introduces a
required module import. Other unused placeholder math functions in this small
libc remain unsuitable as general-purpose libm implementations; new consumers
should receive real implementations and corresponding WASM tests.

### 4. Provide elapsed time for WASM card-save deadlines

Files: [smartmedia.c](../src/smartmedia.c),
[WASM time header](../wasm/libc/include/time.h),
[web imports](../web/gp32-wasm.js).

The full WASM build failed at `smc_host_time_ms`: its branch used `clock()` and
`CLOCKS_PER_SEC`, neither supplied by this freestanding libc. The helper serves
elapsed-time save deadlines. It now calls the declared `pcfx.now_ms` import,
backed by browser `performance.now()`. Tests verify the imported value and
values beyond 32-bit milliseconds.

This restores compilation and gives the helper appropriate elapsed-time
semantics. The current production WASM link discards the unused card autosave
call chain and its clock import; the test module deliberately retains the
helper. This change does not add browser autosave functionality by itself.

### 5. Reuse and release WASM VFS write storage

Files: [freestanding libc](../wasm/libc/src/pcfx_wasm_libc.c),
[WASM regression fixture](../tests/wasm_runtime_test.c).

Three ownership paths lost allocations: growing `fwrite` replaced the old
buffer without releasing it; failed `fclose` publication into a full VFS lost
the writer's buffer; and clearing the VFS erased open writers without freeing
their unpublished storage.

Growth now uses `realloc`, preserving the original data on allocation failure.
Failed publication releases its unpublished storage. VFS clear releases open
writers separately from published owned entries. Readers and caller-owned
borrowed input retain their ownership rules.

Actual-WASM tests verify file contents across growth, cleanup after removal,
ENOSPC publication failure, clear with open readers/writers and borrowed input,
and preservation of an existing byte after allocation failure. The baseline
leaked 520,336, 32, and 144 heap bytes in the three corresponding small tests.
All return to their expected heap positions after the fix.

The complete WASM-side save-generation and staging path used by the browser is
also exercised with a synthetic 32-byte BIOS and emulator RAM, without executing
game code. It creates a 9,290,562-byte state
32 times. The fixed allocator's heap-used value stays at 38,078,512 bytes after
the initial save. The old VFS grows beyond that point during repeated saves. This metric is
`heap_top - heap_base`, including holes below the top; it is not browser RSS or
the WebAssembly linear-memory size. The module still starts with 128 MiB of
linear memory and permits growth to 256 MiB.

### Regression fixture: actually execute both Pinball instruction versions natively

File: [gp32_pinball_fix_test.c](../tests/gp32_pinball_fix_test.c).

Enabling CPU profiling exposed a pre-existing test failure. The fixture ran
the original and corrected instruction blocks once each but expected native
execution, despite the first-encounter portable policy. Its direct self-branch
could also extend the trace beyond the supplied budget. Relinking the exact
baseline core reproduced the same failure.

The fixture now bounds the three-instruction trace with `BX r12`, runs each
version twice, and independently checks native execution before and after the
patch. Both cold and mature execution must produce the correct value and PC.
The game's production patch and its enable/disable behavior are unchanged.

## Verification

| Configuration | Baseline | Integrated changes |
|---|---|---|
| Linux x86-64 Release CTest, default profiling off | 40/40 | New coverage checked with profiling enabled below |
| Linux AArch64 Release CTest under QEMU, default profiling off | 40/40 | New coverage checked with profiling enabled below |
| Linux x86-64 Release CTest, CPU profiling enabled | Existing Pinball coverage failure reproduced | 40/40 |
| Linux AArch64 Release CTest under QEMU, CPU profiling enabled | Focused pre-fix failures reproduced | 40/40 |
| Actual WASM module, Clang/LLD 18 and Node | Audio/ownership regressions reproduced; full build had missing clock API | Build, imports, audio, clock, VFS and repeated saves pass |

An independent reviewer also inspected the integrated changes and reran the
targeted JIT-return, exception, Pinball, and WASM tests. The production web
import object instantiates both the production and test WASM modules. JavaScript
syntax checks and `git diff --check` pass. Existing native compiler warnings
remain; this change set does not claim a warning-free build.

Versions used: GCC and AArch64 GCC 13.3.0, CMake 4.4.4, Clang/LLD 18.1.3, and
Node 24.19.0. QEMU execution checks the Linux AArch64 code path, including emitted
instructions; physical iOS JIT activation, Apple integration, thermal behavior,
audio hardware, and real-device performance require their own validation.

Reproduce the integrated native suite from the repository root:

```bash
cmake -S . -B build-review -DCMAKE_BUILD_TYPE=Release \
  -DGP32EMU_BUILD_TESTS=ON -DGP32EMU_BUILD_BENCHMARK=ON \
  -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_CPU_PROFILE=ON
cmake --build build-review --parallel 4
ctest --test-dir build-review --output-on-failure --parallel 4
```

For Linux AArch64 emulation:

```bash
cmake -S . -B build-review-a64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  '-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-aarch64;-L;/usr/aarch64-linux-gnu' \
  -DGP32EMU_BUILD_TESTS=ON -DGP32EMU_BUILD_BENCHMARK=ON \
  -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_CPU_PROFILE=ON
cmake --build build-review-a64 --parallel 4
ctest --test-dir build-review-a64 --output-on-failure --parallel 4
```

For the freestanding WASM runtime:

```bash
make -f Makefile.wasm build test WASM_CC=clang-18 WASM_LD=wasm-ld-18 -j4
node --check web/gp32-wasm.js
node --check tests/wasm_runtime_test.mjs
```

## Remaining correctness priorities

These issues were reproduced or identified separately during the original
review. Follow-up status is marked where a later change addresses them. Passing
regression suites cover their existing cases; they do not establish complete
architectural emulation.

### Resolved in follow-up: CPU stores can leave stale code while CP15.I is clear

With CP15 c1 at `0x70`, an indirect call executes `MOV r0,#0x11`, a guest STR
replaces that word with `MOV r0,#0x22`, and a second indirect call still returns
0x11 in both cached portable and native execution. The raw interpreter returns
0x22; RAM contains the replacement word in every mode. This is a confirmed
instruction-coherency issue, with no identified affected game in this review.

Raw `wb*`, portable `arm_bc_st_*_phys`, and emitted native stores need coordinated
tracking. The existing physical code-page bitmap and external-write notification
are useful starting points. With I-cache off, ordinary data-page stores should
remain cheap, while a store touching translated code must invalidate and leave
the current trace after retiring the complete guest instruction and writeback.
MMU aliases, successful fetch revalidation after remaps, block transfers, and
exchanges must participate. Invalidating only the next dispatcher lookup misses
modified instructions already inside the current trace. A blanket flush on
every RAM store would introduce substantial avoidable work.

Reference: DDI 0151C, 4.2.2–4.2.3, page 4-6. The existing
[external code-write tests](../tests/arm_code_write_test.c) already expect fresh
fetches with I-cache disabled, but exercise DMA/HLE writers.

The follow-up implements physical instruction-word watches, coordinated raw and
cached store notifications, and native guards before mutation on both backends.
Its 88-scenario regression covers active traces, remaps, all STM lanes, and
independently mapped physical fragments. This correctness fix has a measured
store-throughput cost and a 256 KiB plus padding watch array; the
[CPU code-write report](CPU-CODE-WRITES.md) records those costs and the rejected
coarser prototypes. A full I/D-cache and pipeline model remains outside its scope.

### P2: WFI currently falls through

At PC 0x400, `MCR p15,0,r0,c7,c0,4` followed by `MOV r1,#0x42` and a loop executes
the marker before an interrupt in raw, cached portable, and native settings.
An injected IRQ then records LR 0x40c rather than the documented WFI return
address 0x408. The c7 handler currently treats WFI as maintenance only.

A fix needs a distinct waiting state, interrupt wake, idle time advancing to
SoC deadlines, and saved-state/callback continuation handling. Reusing the
persistent `halted` bit without changes can prevent IRQ handling and timer
progress, because the SoC advances by the CPU's returned cycle count. This is
also a possible idle-work reduction for software that actually uses WFI.
Reference: DDI 0151C, 4.9, page 4-23.

### P2: nonzero FCSE PID is stored but does not modify addresses

With MMU enabled, c13=`0x02000000`, and VA=`0x800`, separate section mappings
produce data and instruction PA=`0x0c100800`, although the FCSE-modified address
selects PA=`0x0c200800`. Both walkers currently use the original VA. A complete
fix must coordinate modified-address tags, fetch proofs, invalidation, and the
documented two-instruction transition after writing c13. Reference: DDI 0151C,
2.3.16, pages 2-24–2-25.

The MMU walker also retains explicit compatibility approximations for invalid
descriptors, access permissions, and domain protection. Strict abort behavior
needs a deliberate compatibility decision and precise exception tests.

### Resolved in follow-up: cached stores can bypass write-fastmem permission

A synthetic bus that supplies read fast memory but rejects writable fast memory
receives the correct write callback in raw execution. Cached portable execution
and native fallback instead write directly through the read-derived RAM base.
The probe observes zero write callbacks and changed RAM where the raw reference
records one rejected write and unchanged RAM.

The ordinary GP32 SDRAM bus is writable, so this proves a CPU/bus contract gap
without establishing a current game failure. A separately authorized write
base, refreshed with mapping changes, would preserve fast ordinary stores and
route restricted writes through the correct callback.

Implemented in the follow-up with a separate write certificate, lazy reset/state
acquisition, and generation invalidation when write mappings change. The new
`arm_write_permission` regression covers denied/partial/split-backed windows and
permission changes within callbacks. This does not complete MMU AP/domain faults.

### Frontend follow-ups

- In the iOS bridge, a host signal probe fills the retained ring with +16,000,
  discards -16,000 during overflow, and resumes -16,000 after freeing space.
  Recovery can use the resampler's discarded tail and leave a 32,000-unit seam
  at the retained/new boundary. Validate a corrected fade anchor with queue
  tests and device audio before changing recovery.
- `GP32Engine::setJitEnabled` in the Qt frontend calls `createMachine`, which
  destroys and reloads the current machine. Review in-place JIT switching and
  audio queue coordination so a setting change preserves session progress.
  This finding is based on code inspection; no Qt UI was run.

## Performance work worth measuring next

### 1. Offer a measured memory profile for small devices

Each decoded slot reserves 96 operations of 20 bytes. Operation storage alone
is therefore:

| Block slots | Decoded-operation capacity |
|---:|---:|
| 4,096 | 7.5 MiB |
| 8,192 | 15 MiB |
| 16,384, native default | 30 MiB |
| 32,768, current WASM build | 60 MiB |

At 16,384 slots, revalidation proofs add 6.6875 MiB and block/poll metadata about
0.53125 MiB on the reviewed 64-bit layout. The default native executable arena
has 64 MiB capacity. These are allocation capacities, not observed resident
memory. Existing `ARM_JIT_BLOCK_COUNT` and `ARM_JIT_CODE_SIZE` overrides permit
controlled experiments without changing the decoder.

Start by comparing 8,192 and 16,384 slots on an affected device. Measure RSS,
block conflicts, compilation counts, recycling, and frame-time tails. Reduced
capacity can save metadata memory and can also increase recompilation. Variable
size operation storage is a larger alternative: native helpers retain decoded
operation pointers, so storage must remain stable for each block's lifetime.

### 2. Use Thumb workload share to decide on a decoded Thumb path

`arm920t_run` sends ARM state through block execution and Thumb state through
individual `exec_thumb` calls. The existing `interp_thumb_insns` counter can
show whether this matters for a slow title. A decoded Thumb path needs correct
BL halfwords, interworking, exception/deadline boundaries, and code invalidation.
Its value should be established from representative workloads first.

### 3. FIR accumulation: exact x86 SSE2 added in follow-up

The shared resampler's main stereo tap loop remains scalar in the reviewed
GCC 13 x86-64 and AArch64 compiler output. An exact vector implementation is a
candidate if audio occupies material CPU time. Preserve the existing 64-bit
accumulation; signed filter lobes and full-scale inputs make an unchecked
32-bit replacement unsafe. Compare every output sample over history boundaries,
rate changes, clipping extremes, and short input blocks.

The follow-up implements exact wide SSE2 accumulation for both the contiguous
source and history paths. It records complete PCM/state comparisons, independent
mathematical checks, and 15-condition repeated GCC/Clang timings. AArch64 retains
the scalar path; real ARM measurements are still needed before adding NEON.

The exact output-count helper copies a 33,584-byte resampler, but its sole
production caller uses it near libretro queue capacity. That makes it a lower
priority than a proven steady-state hotspot.

## Practical device benchmark protocol

Use the existing benchmark's identical state/input replay, CPU profile, and
frame-time output. For example, with local firmware/game assets:

```bash
./build-review/gp32_bench --bios bios.bin --smc game.smc \
  --state scene.state --input-script scene-input.txt \
  --warmup 2400 --frames 600 --jit --cpu-profile --frame-times
```

Run baseline and candidate repeatedly on the same device, scene, clock/power
configuration, and display/audio settings. Compare video/audio hashes and
relevant state before interpreting timing differences. Record p50/p99/max frame
times, frames exceeding 16.67/33.3 ms, audio underruns, peak RSS, and JIT
compilation/invalidation counters. Include loading, steady play, scene changes,
and repeated save/load, followed by sustained play for thermal behavior.

For CPU correctness work, use three references: raw interpreter, cached portable
execution, and native execution. `jit=0` still uses decoded ARM blocks. A bug
shared by cached portable and native paths can therefore pass a two-mode
comparison. In the regression fixtures, tracing with a null callback forces
raw interpretation without producing a trace log.

## Architecture and API references

- [ARM920T Technical Reference Manual, ARM DDI 0151C](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb)
- [ARM Architecture Reference Manual, ARM DDI 0100I](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a)
- [W3C High Resolution Time: performance.now](https://www.w3.org/TR/hr-time-3/#dom-performance-now)

This report's architectural claims use those primary references. Its failure
counts, PCM values, heap observations, and compiler findings come from the
synthetic source-level experiments and regression tests described above.
