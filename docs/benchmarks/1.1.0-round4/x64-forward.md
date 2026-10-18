# x64 forward branches inside a native block

## Decision and measured costs

Adopt the bounded x64 optimization. A conditional forward branch can jump
directly to a later instruction in the same decoded straight-line block.
The tested taken paths used 9.08–37.40% less time across GCC 13 and Clang 18,
resident MMU mappings, four skip widths, and three transparent-pixel densities.
All 126 paired taken-path comparisons favored the change. This is a synthetic
native-host result; it does not establish game or physical AArch64 performance.

The cost is explicit: an eligible branch that was never taken changed median
time by -0.75% to +1.67%. Repeated collection plus code emission cost +5.87% with
GCC and +8.51% with Clang. Unchanged controls also moved, including +5.00% for
Clang's cold no-forward case, so these values should be read alongside their
controls. No attempt was made to subtract control shifts or discard outliers.

### Untaken, no-forward, interpreter, and compilation controls

Values are median nanoseconds per pixel, except the last two rows, which are
nanoseconds per collection-plus-emission. Change is (after / before - 1) × 100;
negative values mean less time.

| Case | GCC before | GCC after | Change | Clang before | Clang after | Change |
|---|---:|---:|---:|---:|---:|---:|
| Eligible but untaken, MMU off | 8.114 | 8.053 | -0.75% | 8.923 | 9.010 | +0.98% |
| Eligible but untaken, MMU on | 10.773 | 10.831 | +0.53% | 11.534 | 11.727 | +1.67% |
| No forward branch, MMU off | 8.221 | 8.216 | -0.06% | 8.845 | 8.874 | +0.33% |
| No forward branch, MMU on | 10.667 | 10.596 | -0.66% | 11.265 | 10.962 | -2.69% |
| Cached interpreter control, 50% transparent | 53.981 | 52.202 | -3.30% | 60.099 | 61.479 | +2.30% |
| Raw interpreter control, 50% transparent | 82.881 | 79.133 | -4.52% | 75.912 | 76.534 | +0.82% |
| Cold collection + emission, no forward branch | 881.107 | 872.324 | -1.00% | 768.705 | 807.114 | +5.00% |
| Cold collection + emission, eligible branch | 878.475 | 930.035 | +5.87% | 827.598 | 898.054 | +8.51% |

Both cold cases use MMU off. Each discards the decoded/native entry and recompiles it while
retaining the arena allocation. It executes no guest instructions and checks
that PC, cycle count, and SWI count remain unchanged. It measures collection
and emission, including the additional bounded eligibility/join scans. It
does not measure process startup, first arena allocation, or a complete
application launch.

### Taken paths

Width is the number of useful opaque-path ARM operations skipped for a
transparent byte. Width 1 contains a byte store; larger widths also update
counts and checksums. The program does not pad the path with dummy operations.

| Case | GCC before | GCC after | Change | Clang before | Clang after | Change |
|---|---:|---:|---:|---:|---:|---:|
| 25% transparent, width 2, MMU off | 8.922 | 7.945 | -10.95% | 9.821 | 8.557 | -12.87% |
| 50% transparent, width 2, MMU off | 9.468 | 7.495 | -20.85% | 10.602 | 8.380 | -20.96% |
| 100% transparent, width 2, MMU off | 10.826 | 6.824 | -36.97% | 12.066 | 7.553 | -37.40% |
| 25% transparent, width 2, MMU on | 11.389 | 10.294 | -9.61% | 12.101 | 11.003 | -9.08% |
| 50% transparent, width 2, MMU on | 11.541 | 9.825 | -14.87% | 12.451 | 10.228 | -17.86% |
| 100% transparent, width 2, MMU on | 12.239 | 8.262 | -32.49% | 13.135 | 8.686 | -33.87% |
| 50% transparent, width 1, MMU off | 9.504 | 7.538 | -20.69% | 10.583 | 8.300 | -21.57% |
| 50% transparent, width 4, MMU off | 9.798 | 7.627 | -22.16% | 10.638 | 8.314 | -21.85% |
| 50% transparent, width 8, MMU off | 10.160 | 8.105 | -20.23% | 11.267 | 8.812 | -21.80% |

All medians use seven samples per version. Every original sample, including
noisy controls, remains in [GCC data](x64-forward-gcc.json) and
[Clang data](x64-forward-clang.json). Individual pairs contain outliers, so the
small control and untaken changes should not be treated as precise universal
effects.

## Why the native path is cheaper

The earlier x64 backend returned to the dispatcher when an eligible forward
branch was taken. Architectural state was already correct. The new path
keeps that branch inside one native entry and returns the number of
instructions actually executed. It reuses the existing A64 proof that the
target and all intermediate operations occupy consecutive guest addresses.
Poll blocks, trace/stop barriers, linked branches, and unsupported condition
encodings keep their existing boundaries.

Only blocks with a real forward skip initialize the saved R15 register.
R15D holds the negative number of skipped operations modulo 2^32. Each exit
uses LEA to add that negative count to its decoded prefix and returns the low
32 bits. The counter is bounded by the decoded block size. The entry still
requires a budget for the complete block, so a shortened path fits that
budget without an additional deadline check. The insufficient-budget return
is emitted before counter initialization and returns an ordinary zero.

All helpers preserve R15 under the existing saved-register frame. Joined
edges invalidate EAX guest-value forwarding, and constant folding cannot
consume a pending branch target. Every normal, helper, cold, and terminal exit
uses the same return-count emitter. A64 uses the shared eligibility helper
with the same proof body as before. Profiling metadata now marks forward
skips as variable counts as well as loops, so it does not mistake an executed
count for an unrelated decoded exit opcode.

### Native coverage and generated code controls

The separate profiling build suppresses clock reads and validates actual
native instruction coverage. For two passes over 1,024 pixels with width 2:

| Transparent pixels | Retired instructions | Before native entries | After native entries |
|---:|---:|---:|---:|
| 0% | 16,386 | 2,048 | 2,048 |
| 25% | 15,362 | 2,560 | 2,048 |
| 50% | 14,338 | 3,072 | 2,048 |
| 100% | 12,290 | 4,096 | 2,048 |

These counts hold with MMU off and on. Native retired instructions equal the
complete returned count in both versions, and the output bytes and registers
are independently checked. The retained
[profile verification](x64-forward-profile-verify.json) covers all 17 cases,
including cached/raw and cold controls. Profiling is disabled in all timed
binaries; their zero-valued profiling fields do not imply a lack of native
execution.

With profiling disabled, the no-forward controls have identical generated
code sizes: 530 bytes with MMU off and 790 with MMU on. The eligible untaken
case changes 565 to 553 bytes and 825 to 813 bytes. When a skip is taken, the
baseline also compiles the target entry; total generated code for width 2 is
789 versus 553 bytes with MMU off and 1,045 versus 813 with MMU on. These are
arena-used byte counts after warm-up, not a claim that all emitted bytes,
including host helper addresses, are identical across separate binaries.

## Correctness evidence

The new focused selector is --internal-forward in tests/arm_jit_test.c.
It includes 512 predicate scenarios (16 conditions × 16 NZCV combinations ×
two target distances), 16 join scenarios (two shapes × two MMU settings ×
four path combinations), and nine callback-stop cases with ordinary and
near-UINT32_MAX budgets, plus the existing transparent-copy and ragged-budget
cases.

The new cases use a trace-forced raw ARM interpreter as the independent
execution path. They check registers, PC, CPSR, returned and cumulative
instruction counts, memory, callback count and observed PC, native coverage,
and profiling attribution. They cover consecutive skips across a helper that
continues, a later helper that stops, entry into a foldable constant chain,
a bypassed EAX producer, terminal SWI handling, all compound predicates, and
direct native calls with zero and insufficient budgets. The ARMv5 BLX encoding
boundary remains a separate control and does not acquire forward eligibility.

| Gate | Result |
|---|---|
| [GCC 13 x64 full differential suite](evidence/x64-forward-jit-x64.log), CPU profiling on | PASS; 716,432 JIT events, zero fallbacks |
| [AArch64 GCC/QEMU full differential suite](evidence/x64-forward-jit-a64.log), CPU profiling on | PASS; 115,386 JIT events, zero fallbacks |
| [Clang 18 x64 focused suite](evidence/x64-forward-clang-focused.log), CPU profiling on | PASS; 4,546 JIT events, zero fallbacks |
| [GCC x64 ASan + UBSan focused suite](evidence/x64-forward-sanitize-focused.log), CPU profiling on | PASS; 4,546 JIT events, zero fallbacks |
| [GCC benchmark verification](x64-forward-gcc-verify.json), clock suppressed | All 34 before/after samples valid |
| [Clang benchmark verification](x64-forward-clang-verify.json), clock suppressed | All 34 before/after samples valid |
| [Native profiling verification](x64-forward-profile-verify.json), clock suppressed | All 34 before/after samples valid |

The final x64 lowering uses a negative counter plus one LEA per exit. The
GCC full, Clang focused, sanitizer focused, and all timing/benchmark
verification binaries include that lowering. The A64 full run preceded the
final x64-only lowering; the shared/A64 source did not change afterward.
Win64 frame and REX/ModRM encoding were reviewed statically; no Win64 runtime
execution is claimed. ASan and UBSan ran with leak scanning disabled because
LeakSanitizer cannot access the required process information in this sandbox.
QEMU results are functional evidence and are not AArch64 timing results.

## Measurement and reconstruction

The actual host was AMD EPYC 9V74 80-Core Processor, Linux x86-64, glibc 2.39.
The runner pinned itself and its children to allowed CPU 0. GCC was
gcc-13 (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; Clang was Ubuntu clang version
18.1.3 (1ubuntu1). No builds, QEMU runs, or other agent tests overlapped the
controlled timing window. Host scheduling and frequency were not otherwise
controlled.

Each execution sample uses 32,768 deterministic input bytes, two untimed
warm-up passes, and 64 measured passes. Cold samples use two warm-up
compilations and 5,000 measured compilations. Before and after alternate
execution order in successive repetitions. GCC's complete run preceded
Clang's. There are 17 cases × seven repetitions × two versions × two
compilers = **476 timing samples**, all checked. The three clock-suppressed
verification runs retain **102 additional samples**.

The isolated measured source is local ebaa61a51b10740c46e6f04ffe29ef30171f8d2a before and
82fd1980b6835aa5decc127445f31073557e4182 after. It includes the callback-fetch fix on b141220 and the
forward-branch change. The combined round-four branch also contains independent
fixes. The exact arm920t.c SHA-256 values are:

| Source | SHA-256 |
|---|---|
| Before | ec5e28cfe3f6a311d2bbe6a0233c44e2c0cac83d40995d9c46ca72a1232c4d5d |
| After | 4156b8a7381c660480f0e6ef265639019182c39deb12e1573b640cec1c3eac68 |

The [manifest](evidence/x64-forward-manifest.json) records source, binary, and
evidence hashes, compiler flags, and integration commit identities. Local fetch
integration 618e6600aa201ac182498c22a301b8ec6c03f1ae and forward integration
06c4d879543f90d1003d174718d738e27a8ed6b8 remain records of the original audit history.
Reproduction uses the public b141220 source and the exported
[baseline patch](isolation/x64-forward-baseline.patch), followed by the
[candidate patch](isolation/x64-forward-candidate.patch). The
[reconstruction record](evidence/isolation-reconstruction.json) verifies the
resulting source bytes and patch hashes. This yields the measured before and
after sources independently of the additional fixes in the integrated tree.

Run the following from the current repository root, using fresh temporary
destinations. Only the public base commit and the checked-in patch files are
needed to reconstruct the two measured source trees. Use the benchmark drivers
from the current repository:

```sh
gp32_forward_patch_dir="$PWD/docs/benchmarks/1.1.0-round4/isolation"
mkdir -p /tmp/gp32-forward-before
git archive b141220cd5fd9d628305133fc963ed86a30f7e41 src include | tar -x -C /tmp/gp32-forward-before
git -C /tmp/gp32-forward-before apply "$gp32_forward_patch_dir/x64-forward-baseline.patch"
cp -a /tmp/gp32-forward-before /tmp/gp32-forward-after
git -C /tmp/gp32-forward-after apply "$gp32_forward_patch_dir/x64-forward-candidate.patch"
```

The timed builds used -O3 and -D_XOPEN_SOURCE=700, with -std=c2x for GCC and
-std=c23 for Clang. CPU profiling defaults to zero in the fixture; -DNDEBUG
was not added. The original build used -Isrc -Iinclude and the default
after include, with CORE_UNDER_TEST pointing to the exported baseline for
the before binary. These equivalent commands select the reconstructed trees:

```sh
gcc-13 -std=c2x -O3 -D_XOPEN_SOURCE=700 \
  -I/tmp/gp32-forward-before/src -I/tmp/gp32-forward-before/include \
  '-DCORE_UNDER_TEST="/tmp/gp32-forward-before/src/arm920t.c"' \
  scripts/bench_forward_branches.c -o /tmp/forward-gcc-before
gcc-13 -std=c2x -O3 -D_XOPEN_SOURCE=700 \
  -I/tmp/gp32-forward-after/src -I/tmp/gp32-forward-after/include \
  '-DCORE_UNDER_TEST="/tmp/gp32-forward-after/src/arm920t.c"' \
  scripts/bench_forward_branches.c -o /tmp/forward-gcc-after
python3 scripts/bench_forward_branches.py \
  /tmp/forward-gcc-before /tmp/forward-gcc-after \
  /tmp/forward-gcc.json --repetitions 7
```

For Clang, use clang-18 -std=c23 in both builds and run its pair sequentially.
A separate GCC -O2 -DGP32EMU_CPU_PROFILE=1 pair provides native coverage; run
the same Python driver with --verify --pixels 1024 --passes 2
--compile-passes 2 --repetitions 1 to suppress all clock reads. The raw sample
files retain binary hashes for the actual measured executables, which need
not match binaries rebuilt with different include paths or toolchains.
