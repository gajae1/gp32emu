# Round 5 interpreter and memory audit

## Baseline and scope

This continuation starts at local commit
`85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f`, equivalent to public commit
`2bace9afd9c3d1e5e7cac9d23379e2f60d167fec` at tree
`396aaf687ce5d1ead21db9af44f25185b44ec698`. The earlier review and rejected
experiment reports were read before selecting this work. No physical device,
game corpus, release, or runtime performance claim is involved.

The fresh GCC 13.3 Release configuration requires C23, enables tests and CPU
profiling, and disables IPO. All 54 pre-existing CTest entries passed before
changes. Compiler commands and source identities are retained below; timings
of these functional checks are not benchmarks.

## Accepted: honor direct mappings inside the I/O address window

`arm_bus_t.fastmem` independently authorizes reads and writes of each complete
physical span. A generic bus may provide direct memory inside
`0x14000000..0x16000000`; recognizing this address window does not prove that
the bus wants a callback. The raw accessors already probe `fastmem` first.
The six cached physical accessors previously selected the I/O callback before
making that probe, so valid direct mappings could read or modify a different
backing store depending on the executor.

The change is confined to `arm_bc_ld_word_phys`, `arm_bc_ld_byte_phys`,
`arm_bc_ld_half_phys`, `arm_bc_st_word_phys`, `arm_bc_st_byte_phys`, and
`arm_bc_st_half_phys`. Existing whole-RAM/BIOS direct proofs are retained.
Otherwise the helpers probe the already translated physical address for the
actual width and direction, then use the appropriate ordinary or optional
word-I/O callback on a miss. No address is translated twice.

Native word-I/O routes have the same independent obligation. Their companion
fix and separate certificate regression are documented in
[`../jit/README.md`](../jit/README.md). The native change preserves its trusted
SoC helper path and uses per-access mapping probes unless the bus's
`fastmem_excludes` query proves that the complete I/O range has no direct
mapping in that direction. `open_bus_valid` alone is insufficient:
the public certificate permits contiguous RAM extending beyond the standard
8 MiB window. Integrate that native change before this new CTest entry, or
the new regression intentionally continues to expose the native discrepancy.

### Independent regression and before/after evidence

`tests/arm_fastmem_io_test.c` contains two deliberately different backing
arrays, explicit per-direction mapping permissions, and a bounded direct
span. Expected bytes, loaded registers, callback counts, writeback, unchanged
registers, CPSR, and retirement come from literal fixture data and arithmetic;
they do not use another CPU executor as the oracle. Full BIOS/RAM mappings
and an explicitly prepared native block provide positive native coverage.

The 2,478 cases comprise 826 cases for each of raw, cached, and native-requested
execution. They cover 17 load/store/swap/multiple-transfer forms, all four
read/write mapping policies, three direct-span limits, unaligned word rotation,
post-index writeback, optional callback absence, I/O-window boundary crossings,
MMU-off/identity-section/aliased-section access, and a certified ordinary-bus
control. Guest MCR instructions establish the MMU configurations.

| Exact source under test | Cases | Raw assertion failures | Cached assertion failures | Native assertion failures | Native instructions |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline, GCC 13.3 | 2,478 | 0 | 932 | 932 | 1,576 |
| Six cached helpers only, GCC 13.3 | 2,478 | 0 | 0 | 105 | 1,576 |
| Combined cached/native fix, GCC 13.3 x64 | 2,478 | 0 | 0 | 0 | 1,576 |
| Combined cached/native fix, GCC AArch64 under QEMU | 2,478 | 0 | 0 | 0 | 1,576 |
| Combined cached/native fix, Clang 18 x64 | 2,478 | 0 | 0 | 0 | 1,576 |
| Combined cached/native fix, GCC 13.3 x64 UBSan | 2,478 | 0 | 0 | 0 | 1,576 |

The retained expanded baseline and cached-only logs separate the common-helper
and special native routes. The first exploratory 132-case log is historical;
the expanded fixture and its exact source hash are the reproducible regression.
The native agent retained the GCC x64/AArch64 commands and raw output in
`../jit/fastmem-full-combined-{x64,a64}.log`. Its 12-case certificate regression
also checks an extended contiguous mapping and independent read/write proofs,
including exact probe counts on the ordinary trusted path.

`fastmem-combined-sources.json` records the exact temporary source combination:
the six cached helpers copied verbatim into the companion native source, with
the matching AArch64 include. This is a functional integration check, not a
claim that either isolated commit alone fixes both executor paths.
That historical combination used provisional endpoint-based certification.
Subsequent review rejected the inference because fastmem permissions can have
direction- or width-dependent holes. The final native companion uses an
`fastmem_excludes` cold-range proof; its separate hole and width controls are required
in addition to the bounded mapping cases recorded here. The historical
source identity and passing output are retained without claiming that the
earlier certificate was sufficient for all permitted buses.

### Commands and identities

Run from the repository root. In this audit `$audit_build` was
`/tmp/gp32-round5-build-interpreter` and the CMake/CTest executables were from
`/tmp/gp32-round4-tools/cmake/data/bin/`.

```sh
audit_build=/tmp/gp32-round5-build-interpreter
/tmp/gp32-round4-tools/cmake/data/bin/cmake --fresh -S . -B "$audit_build" \
  -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc \
  -DGP32EMU_REQUIRE_C23=ON -DGP32EMU_BUILD_TESTS=ON \
  -DGP32EMU_BUILD_LIBRETRO=ON -DGP32EMU_CPU_PROFILE=ON \
  -DGP32EMU_ENABLE_IPO=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
/tmp/gp32-round4-tools/cmake/data/bin/cmake --build "$audit_build" -j 2
/tmp/gp32-round4-tools/cmake/data/bin/ctest --test-dir "$audit_build" --output-on-failure -j 2

# Before: exact baseline source, the final expanded fixture.
git show 85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f:src/arm920t.c > "$audit_build/arm920t-before.c"
gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -DGP32EMU_CPU_PROFILE=1 \
  '-DCORE_UNDER_TEST="/tmp/gp32-round5-build-interpreter/arm920t-before.c"' \
  -Isrc -Iinclude tests/arm_fastmem_io_test.c -o "$audit_build/arm_fastmem_io_before"
"$audit_build/arm_fastmem_io_before"

# Cached helper change in isolation; the remaining native failures are expected.
gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -DGP32EMU_CPU_PROFILE=1 \
  -Isrc -Iinclude tests/arm_fastmem_io_test.c -o "$audit_build/arm_fastmem_io_bc_after"
"$audit_build/arm_fastmem_io_bc_after"

# Exact combined source recorded in fastmem-combined-sources.json.
clang-18 -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -DGP32EMU_CPU_PROFILE=1 \
  '-DCORE_UNDER_TEST="/tmp/gp32-round5-jit-fastmem-combined/arm920t.c"' \
  -Isrc -Iinclude tests/arm_fastmem_io_test.c -o "$audit_build/arm_fastmem_io_combined_clang"
"$audit_build/arm_fastmem_io_combined_clang"
gcc -std=c2x -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
  -D_XOPEN_SOURCE=700 -DGP32EMU_CPU_PROFILE=1 \
  '-DCORE_UNDER_TEST="/tmp/gp32-round5-jit-fastmem-combined/arm920t.c"' \
  -Isrc -Iinclude tests/arm_fastmem_io_test.c -o "$audit_build/arm_fastmem_io_combined_ubsan"
"$audit_build/arm_fastmem_io_combined_ubsan"
```

| File at the bounded memory-fix checkpoint | SHA-256 |
| --- | --- |
| Baseline `src/arm920t.c` | `decf0d2f925058f00391a46d335245b06ada19152e35a817415e17873e844cf3` |
| Cached-helper-only `src/arm920t.c` | `d2190e368518d39ce6eb9f46848f9efed045aeb8f5652e64c35d1f80a526ce53` |
| `tests/arm_fastmem_io_test.c` | `a5c0e9593ad7847725f9ecf2ca38e4f9c321b59f8a560ed9f0ff4581a96f6598` |
| Combined `arm920t.c` | `c1c0f2b5edc8dbdad2815ed97411883a4c0fd71d4f1a9b0179793cf2eac7e491` |

The first Ninja configure failed because Ninja was not on PATH; the fresh
Unix Makefiles configure above succeeded. The unversioned `clang` command was
also absent; the installed `clang-18` executable was used. Neither setup
failure changed the implementation or an oracle expectation.

## Review of external-write watch refinement

The companion HLE work refines a positive external-write page hit using the
existing four-byte instruction watch bitmap. Review found that raw collection,
side-effect-free speculative peeks, and block proof revalidation all mark
instruction words without excluding CP15.I=1. Inlined leaves are inspected
through these paths. Constant DATA folding consumes tracked opcode fields;
AArch64 PC-relative literal address folding still performs runtime memory
loads. Thumb has no decoded/native cache and reads current instruction bytes.
An external halfword write overlapping an ARM word must still invalidate it.

The existing `jit_code_pages_untracked` fallback remains necessary for MMU or
otherwise untracked code. The historical optional experiment
`skip-unused-word-watches.patch`, which omitted I=1 word watches, is incompatible
with this finer external-write policy and must remain historical evidence.

## Independent ARM multiply audit

`tests/arm_multiply_test.c` adds an independent arithmetic oracle for MUL,
MLA, UMULL, UMLAL, SMULL and SMLAL. It checks **872,064 cases**, 290,688 each
through raw, cached and native-requested execution. Inputs include every pair
of 15 zero, sign, carry, halfword and bit-pattern boundaries, accumulator
patterns, permitted source/destination aliases, all 16 initial NZCV values,
S clear/set, and all 14 ordinary conditional predicates. All general-purpose
registers, affected/preserved CPSR bits, retirement and return PC are checked.

The product oracle converts signed operands to unsigned magnitudes and uses
repeated doubling/addition; it does not multiply converted signed operands
as the implementation does. Condition expectations are an explicit boolean
table. The flag and operand exclusions follow
[ARM ARM DDI0100I](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a)
A4.1.34, A4.1.40, A4.1.76, A4.1.87, A4.1.128 and A4.1.129: short multiply's C,
and long multiply's C/V, are not asserted for ARMv4 S=1 forms; S=0 and all
failed conditions preserve every flag. R15, equal long destinations, and
historically restricted Rm/destination aliases are excluded.

GCC 13.3 x64, Clang 18 x64, GCC x64 UBSan and GCC AArch64/QEMU each pass the
complete oracle. Native execution accounts for 581,376 instructions in every
native-enabled build, with zero multiply-kind semantic helpers. AArch64's
test return `BX r12` uses a checked helper: the initial fixture incorrectly
asserted zero helpers of every kind, although every arithmetic result passed.
`multiply-a64-fixture-before.log` retains that one assertion failure. The
corrected check requires zero **multiply** helpers and reports other helpers
separately. This corrects the fixture's backend assumption without weakening
native multiply coverage or changing an arithmetic expected value.

No multiplication implementation change was justified by this audit. The
existing unsigned modular multiplication/accumulation expresses the tested
32/64-bit results without signed-overflow arithmetic. Raw logs are
`multiply-{gcc,clang,ubsan,a64}.log`. Reproduce as follows:

```sh
gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -Isrc -Iinclude \
  tests/arm_multiply_test.c -o /tmp/gp32-round5-build-interpreter/arm_multiply_gcc
/tmp/gp32-round5-build-interpreter/arm_multiply_gcc
clang-18 -std=c23 -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -Isrc -Iinclude \
  tests/arm_multiply_test.c -o /tmp/gp32-round5-build-interpreter/arm_multiply_clang
/tmp/gp32-round5-build-interpreter/arm_multiply_clang
gcc -std=c2x -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
  -D_XOPEN_SOURCE=700 -Isrc -Iinclude tests/arm_multiply_test.c \
  -o /tmp/gp32-round5-build-interpreter/arm_multiply_ubsan
/tmp/gp32-round5-build-interpreter/arm_multiply_ubsan
aarch64-linux-gnu-gcc -std=c2x -O3 -DNDEBUG -D_XOPEN_SOURCE=700 -Isrc -Iinclude \
  tests/arm_multiply_test.c -o /tmp/gp32-round5-build-interpreter/arm_multiply_a64
qemu-aarch64 -L /usr/aarch64-linux-gnu /tmp/gp32-round5-build-interpreter/arm_multiply_a64
```

## Other candidates and host limits

The raw ARM and remaining Thumb PUSH/POP register-list scans were reviewed.
No loop rewrite is adopted without an isolated workload measurement and the
corresponding defined-behavior oracle. In particular, no POP callback timing
change is inferred from final architectural writeback semantics. Prior
accepted Thumb LDMIA/STMIA measurements do not establish the benefit of these
different loops.

The zero-field MSR fallback was also examined. DDI0100I's assembler syntax
requires one or more field suffixes, and no ARM920T-specific guarantee for
the zero-field form was established. The audit does not promote this
ambiguous encoding into a defined no-op or change its existing behavior.

`host32-probe.log` retains a failed `gcc -m32 -std=c2x -fsyntax-only
-DGP32EMU_DISABLE_JIT=1 -Isrc -Iinclude src/arm920t.c` probe. The environment
lacks the multilib `bits/wordsize.h` header, so this round does not claim a
32-bit host build or runtime pass. Successful x64 and AArch64 sanitizer and
functional checks do not substitute for that missing configuration.


## Accepted: notify writes to translated BIOS and other mapped code

The existing CPU write notifier watched only the standard 8 MiB RAM window.
A generic bus may expose writable BIOS or executable memory elsewhere, through
fastmem or ordinary callbacks. A CPU store could successfully replace such
an instruction while a cached or native block continued to execute its old
opcode with CP15.I clear. The external-write API could likewise miss BIOS or
extra code below RAM when no conservative MMU fallback was active.

This is reachable through ordinary public dispatch. A RAM/BIOS caller can
stitch a short `BL` leaf from a different physical region into its cached or
native trace. Merely running a cold extra-region PC is an inadequate test:
that dispatcher route deliberately executes raw. The explanatory comment at
`arm_jit_code_cacheable` now states this distinction.

### Bounded common change and native companion

Two runtime bits record whether fetched code has used BIOS or another
non-RAM physical region. They are appended to the CPU structure, after the
existing watch bitmap, without changing previous field offsets or the
serialized guest state. `arm_jit_track_code` records the physical region at
ordinary collection, speculative direct fetch, and proof revalidation. The
ordinary fetch hook records it after callback reads too; write permission is
not inferred from read permission. Tracking remains enabled with CP15.I=1.
A full generation clears these bits; a lazy epoch preserves them.

With CP15.I=0, `arm920t_cpu_code_write` tests these recorded regions when a
one-, two-, or four-byte physical write lies outside or crosses standard RAM.
The ordinary RAM watch path is retained. BIOS and other regions are separate,
so normal execution from BIOS does not make unrelated MMIO writes renew the
epoch. The existing CP15.I=1 maintenance policy remains unchanged. Checked
cached helpers and native helpers stop continuation when the epoch changes,
so a store inside an active trace cannot reach a stale following instruction.
The common change and final fixture are commit `89184ed`. The x64 and
AArch64 word-I/O helpers require the two companion calls in native commit
`10e30b1`; their snapshot/epoch fence remains in force.

External notifications use the same rare-region bits before the existing
RAM word-watch refinement and untracked/MMU fallback. The public header now
specifies a nonwrapping physical interval clipped at 2^32: callers split a
wrapped DMA span into separate notifications. CPU accessors retain their
existing wrapping 32-bit endpoint semantics for a callback that writes across
the final byte. No guest instruction's alignment semantics are changed.

### Independent oracle and precise coverage

`tests/arm_nonram_code_write_test.c` uses literal `MOV r0,#0x11` and
`MOV r0,#0x22` instructions, explicit expected return PCs and CPSR, separate
physical/virtual aliases, and exact callback counts. It never uses another
executor's result as its expected value. Actual guest MCR instructions set
I-cache and MMU controls. The six CPU forms are STR, STRB, STRH, SWP, SWPB,
and a two-word STM; additional raw ARM and Thumb writers target previously
cached/native code. Prepared cached/native blocks cover regions a cold entry
would deliberately interpret raw. Their native paths have positive execution
checks.

The separate public-dispatch group uses only public CPU entry points to prime
and execute the caller and leaf. Metadata is inspected solely to assert that
the collected caller actually contains the leaf. It covers BIOS, extra mapped
memory below RAM, just below RAM, and just above RAM; MMU aliases, CPU and
external writers, both I-cache settings, and direct/callback stores are
included. Its **192 cases have 56 assertion failures before the rare-region
change and zero afterward**.

| Group | Cases | Isolated before assertion failures | Combined after assertion failures |
| --- | ---: | ---: | ---: |
| Separate CPU replacement, including raw ARM/Thumb writers | 960 | 320 | 0 |
| Active self-modifying trace | 72 | 40 | 0 |
| External byte/halfword/word replacement | 216 | 24 | 0 |
| Ordinary RAM and MMIO data writes | 60 | 0 | 0 |
| Defensive halfword boundary notification | 24 | 6 | 0 |
| Clipped external interval oracle | 9,072 | 3,580 | 0 |
| Public-dispatch stitched BL leaf | 192 | 56 | 0 |
| **Total** | **10,596** | **4,026** | **0** |

The four boundary controls call `wb16` directly and check notification for the
bytes the supplied callback actually writes at the BIOS end, RAM start/end,
and 32-bit wrap. They do not prescribe the result of an odd-address ARMv4
STRH, whose architectural behavior is not used as an oracle. External span
expectations intersect a separate table of mathematical half-open intervals,
using clipped 64-bit sums around all region boundaries, zero length and
UINT32_MAX lengths. Those 3,580 before failures are missed conservative-region
notifications, not 3,580 independently executed stale instructions. The
replacement and public-dispatch groups separately establish observable stale
execution.

### Source separation, validation, and rejected fixture assumptions

The isolated before source retains the earlier external RAM word-watch
refinement and the final native `fastmem_excludes` proof. It reverts only the
rare-region common change and the two native I/O notifier calls. Its raw
executor passes all 476 cases; cached and native each fail 2,013 assertions
across 5,060 cases. `nonram-isolated-before.patch` reconstructs that exact
source from the final combined candidate, and `nonram-isolated-sources.json`
records every CPU/include/header identity plus local test and binary hashes.

GCC 13.3 x64, Clang 18 x64, GCC x64 UBSan, and GCC AArch64 under QEMU pass the
entire 10,596-case fixture. Native-enabled candidates report **4,564 cached
and 2,076 native instructions**. `GP32EMU_DISABLE_JIT=1` passes the same cases
through portable dispatch, reporting 6,640 cached and zero native instructions.
The existing independent RAM external-write test also passes all **12,920**
span, replacement and MMU-fallback cases against the combined source.

The common-only branch intentionally retains seven native I/O failures until
the native mapping and store-notification companions are integrated. Its raw
and cached modes pass every case. No isolated-commit full-suite success is
claimed. Final whole-core integration gates belong to the parent audit.

The initial 1,308-case fixture assumed a cold extra-region PC had entered a
cached block. Its 96 cached-I1 retention failures were a fixture mistake;
the raw fallback was correctly reading current memory. The frozen fixture
`nonram-code-fixture-before.c` and its before/common-only logs are retained.
The correction explicitly prepares the derived cached block, preserves every
instruction expected value, and adds the public-dispatch group above.
`nonram-code-before-final.log` is a 10,596-case historical comparison
against an older combined source that also lacked the RAM word refinement;
its 4,246 failures must not be attributed solely to this rare-region fix.
The isolated 4,026-failure log and the corrected fixture are the final before
proof. `nonram-code-combined-final.log` is the intermediate zero-failure run
before category summaries and before changing the defensive halfword controls
from odd-address STRH to direct `wb16` calls.

Final GCC/Clang/AArch64 combined logs and source snapshots are retained in the
native companion's `../jit/nonram-final-combined-*`. Local Clang, UBSan,
portable, common-only, isolated-before and existing-RAM output is retained as
`nonram-code-{clang,ubsan,portable,common-only,isolated-before}.log` and
`nonram-external-existing.log`. The local sanitizer and Clang candidate source
`3abe682...` differs from final `8e390d7...` only in the corrected cold-dispatch
comment; the matching AArch64 include is byte-identical. Reproduction commands
are in `nonram-commands.sh`. These are functional checks; no performance
claim is made from their duration.
