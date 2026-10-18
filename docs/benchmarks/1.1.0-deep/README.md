# 1.1.0 deep-review measurements and optional experiments

These are isolated synthetic measurements from 2026-10-17/18 UTC, not game FPS,
device battery measurements, or physical AArch64 benchmarks. See the
[Korean deep review](../../REVIEW-1.1.0-DEEP.ko.md) for the correctness findings
and the [earlier measurements](../1.1.0-followup/README.md) for the already
retained SSE2 audio and writable-mapping changes.

[Regression and final-validation logs](evidence/README.md) preserve the failure
reproductions, four default CTest runs, WASM run and isolated optional-patch
checks. [Commit mapping](evidence/commit-map.json) and
[validation metadata](evidence/validation.json) identify the reviewed sources.

## Decisions and evidence inventory

| Change | Default core | Retained evidence |
|---|---|---|
| Disjoint, whole-service, incrementing RAM-to-RAM DMA | **Adopted** | `dma-release-gcc.json`, production code, independent oracle, benchmark drivers |
| Replace the instruction-word watch loop with bitmap range masks | **Not adopted** | `watch-range-exploratory-{gcc,clang}.json`, `candidates/watch-range-loop.patch` |
| Omit unused instruction-word watches while guest I-cache is enabled | **Not adopted; optional patch** | `watch-skip-release-{gcc,clang}-i{0,1}.json`, `candidates/skip-unused-word-watches.patch` |

The default `arm_jit_track_code` still records page and instruction-word watches
with either value of guest `c1.I`. Neither candidate is silently included in the
core. The two candidate patches are alternatives, not a tested combined patch.

The host was a shared x86-64 Linux 6.18.55 virtual environment reporting an
AMD EPYC 9V74 CPU, glibc 2.39, and an 8 GiB memory limit. Timed comparisons used
CPU affinity 0, `CLOCK_MONOTONIC`, adjacent before/after runs with alternating
AB/BA order, and no concurrent builds, tests, or other agent benchmarks during
their timing windows. Affinity and quiet local work do not eliminate host-level
scheduling, frequency, or virtual-machine noise. Reported values are medians;
raw samples are retained so that small differences can be inspected.

| File group | Samples | Configuration |
|---|---:|---|
| `dma-release-gcc.json` | 180 | 18 conditions × 5 repetitions × 2 variants; GCC 13.3 Release, CPU profiling OFF |
| `dma-profile-exploratory.json` | 90 | 15 conditions × 3 repetitions × 2 variants; earlier profiling-ON exploration |
| `watch-range-exploratory-*.json` | 640 total | 32 conditions × 5 repetitions × 2 variants × 2 compilers; profiling ON |
| `watch-skip-release-*-i1.json` | 588 total | 21 conditions × 7 repetitions × 2 variants × 2 compilers; profiling OFF |
| `watch-skip-release-*-i0.json` | 252 total | 9 control conditions × 7 repetitions × 2 variants × 2 compilers; profiling OFF |

The DMA exploration uses a `metadata`/`samples` schema. The final DMA file uses
condition groups under `results`, each containing both variants' sample lists.
The JIT files use one flat `results` list. These are different runs, not pieces
of a single statistical population.

## 1. Adopted RAM DMA fast path

The new path requires whole-service mode, more than one transfer unit, both
addresses incrementing, a defined byte/halfword/word width, complete source and
destination spans in physical RAM, and disjoint ranges. It uses `memcpy` and
then reaches the existing completion tail: code invalidation, current addresses,
count, reload/stop behavior, and IRQ handling. The 20-bit transfer count bounds
the length before range checks.

Fixed-address, overlapping, partial-RAM, MMIO, single-service, one-unit, and
reserved-width cases retain the ordinary loop. In particular, replacing an
overlapping forward DMA with `memmove` would not reproduce sequential
read-one-unit/write-one-unit propagation. The new 4,160-case independent oracle
checks these cases as well as eligible copies; both the old and new code pass.
This change is separate from the pre-existing IIS audio DMA shortcut.

### Release results

`dma-release-gcc.json` is the primary result: GCC 13.3, `-O3 -DNDEBUG -std=c2x`,
`GP32EMU_CPU_PROFILE=OFF`, threads enabled. The C fixture uses the public SoC
bus and attaches a real CPU as the IRQ/code-invalidation sink. It starts every
transfer by writing the software request and ON bits. With reload disabled,
completion clears ON, so the next write starts the programmed original transfer
again. It checks completion registers and the full payload outside the timer;
there is no separate per-iteration transfer counter.

After 64 warmup transfers, each sample targets 128 MiB copied, with a minimum
of 256 and a maximum of 4,000,000 transfers. Sizes are 4, 8, 32, 4,096, 153,600,
and 524,288 bytes for all three widths. The final run lasted about 18.22 seconds,
23:51:37–23:51:55 UTC on October 17. Every sample records the payload hash and
the actual reset clocks: FCLK/PCLK/run clock 48 MHz, speed 100%. No guest
instructions or SoC ticks run inside this synchronous DMA benchmark.

| 153,600-byte transfer | Before, µs | After, µs | Before / after |
|---|---:|---:|---:|
| Byte units | 206.185 | 2.961 | 69.6× |
| Halfword units | 228.461 | 3.130 | 73.0× |
| Word units | 139.411 | 2.929 | 47.6× |

These ratios compare the previous per-unit emulation loop with a guarded host
bulk copy. They are not multipliers for whole-emulator speed. Eligible 8-byte
transfers improved by approximately 1.19–1.53× in this run.

The excluded 4-byte word case costs one extra path decision: its median changed
from **15.627 to 16.032 ns**, approximately **+0.405 ns / +2.6%**. Its five
before samples were 15.511, 15.580, 15.627, 16.249, and 15.664 ns; after samples
were 17.489, 17.113, 15.955, 15.766, and 16.032 ns. This control is disclosed
alongside the large-copy gains; the experiment does not establish a universal
benefit for every DMA workload or a stable cost of exactly 0.405 ns.

### Source and build provenance

Only the SoC object differed between the two measured static libraries. The
baseline source came from isolated commit
`0cbadb43bccfe4b51ef6f316346e3e237816a437`; the candidate from
`ec01ca87d0fdb1a0120b658cf27296d2af405f95`. These are local experiment commits;
the adopted change appears in the review history as **Batch disjoint
whole-service RAM DMA transfers**. Source hashes identify the measured files
independently of local versus published commit metadata:

| Source | SHA-256 |
|---|---|
| Baseline `src/s3c2400.c` | `ada89d61438e20b0893fc2024583c231ae6b194982f5183483ec6cf663a9ac75` |
| Candidate `src/s3c2400.c`, also final reviewed source | `f9b6bb4ff0be243dc3ec096e71bedeb6e398cee2779c72d776b93e2163dec617` |

The candidate library was built in `/tmp/gp32-deep-soc-off-build`. A copy became
`/tmp/gp32-deep-soc-off-before-dma.a`: its `s3c2400.c.o` member was removed and
replaced by the baseline object, leaving every other member unchanged. Baseline
object flags matched the Release build:

```text
-DGP32EMU_ENABLE_THREADS=1 -DZIP_ENABLE_DEFLATE=0 -DZIP_HAVE_SYMLINK=0
-D_FILE_OFFSET_BITS=64 -D_XOPEN_SOURCE=700 -Iinclude -Isrc
-O3 -DNDEBUG -std=c2x -Wall -Wextra -Wpedantic
```

The same retained [C driver](../../../scripts/bench_s3c2400_dma.c) was linked
against each library with `-O3 -DNDEBUG -std=c2x -Wall -Wextra -Iinclude -Isrc
-lm -pthread`. The [Python runner](../../../scripts/bench_s3c2400_dma.py) used
its current defaults and a configuration label spelling out the compiler and
profiling setting. A rerun against two independently prepared libraries is:

```sh
cc -O3 -DNDEBUG -std=c2x -Wall -Wextra -Iinclude -Isrc \
  scripts/bench_s3c2400_dma.c /path/to/before/libgp32emu.a -lm -pthread -o /tmp/dma-before
cc -O3 -DNDEBUG -std=c2x -Wall -Wextra -Iinclude -Isrc \
  scripts/bench_s3c2400_dma.c /path/to/after/libgp32emu.a -lm -pthread -o /tmp/dma-after
python3 scripts/bench_s3c2400_dma.py /tmp/dma-before /tmp/dma-after /tmp/dma-results.json \
  --configuration 'record compiler, exact library flags, CPU profiling and source revisions here'
```

For a controlled comparison on the final review tree, build Release with
`GP32EMU_REQUIRE_C23=ON` and `GP32EMU_CPU_PROFILE=OFF`; create the baseline archive
by replacing only its SoC object with the parent of the named DMA commit.
Other final-review fixes can remain identical on both sides. That reproduces
the comparison method, not the exact historical binaries or host timings.

`dma-profile-exploratory.json` and its log preserve the earlier 23:46:07–23:46:14
UTC run. It used profiling ON, three repetitions, at most 1,000,000 transfers,
five sizes (no 8-byte case), and an earlier scratch driver with fewer untimed
checks and no printed clock fields. Its runner iterated repetitions first.
Do not subtract those values from the final profiling-OFF results or describe
them as repetitions with identical measurement settings.

## 2. Unadopted bitmap range-loop experiment

The [range-loop patch](candidates/watch-range-loop.patch) changes long word-watch
updates to first/last masks and full interior bitmap words, while preserving
the original one-instruction path. A separate randomized, prefilled-bitmap
oracle passed 477/477 cases under GCC UBSan, ending with seed `589e2b68`.

The exploration used GCC 13.3 `-O3 -std=c2x` and Clang 18.1.3 `-O3 -std=c23`,
profiling ON, five samples of 200,000 block executions. Its 32 conditions were
portable/native mode × 2/16/48/96 instructions × 1/64 working blocks ×
unchanged/cache-epoch-changed execution. All 640 samples passed their correctness
and backend-counter checks. This early fixture/runner preceded the additional
untimed readiness and source-stability checks in the retained final drivers.

Long native revalidation cases reduced elapsed time by roughly 27–40%; long
portable cases improved by roughly 7–13%. Small/hot controls were mixed. One
GCC native 48-instruction, 64-block, no-maintenance control was **+7.75%** in
these short samples. Disassembly inspection found identical Clang hot-loop
bytes but changed register allocation in the actual GCC `arm920t_run` body;
hashing only its public wrapper would have missed that difference. This does
not establish the cause of every timing change.

The experiment was not adopted or rerun with the final longer Release protocol.
Its data records why investigation moved to the smaller I-cache-specific
candidate; it is not a demonstrated device-level regression or a reason to
restore an old optimization automatically.

## 3. Optional I-cache-enabled word-watch omission

The [optional mail patch](candidates/skip-unused-word-watches.patch) retains
unconditional page tracking, then returns before the word-watch loop when guest
`c1.I` is set. Word watches are consulted for CPU self-modifying code with I=0;
external DMA/HLE writes still depend on page tracking with I=1. Moving this
return above page tracking would violate the required metadata invariant.

The current implementation keeps `c1.I` constant within a JIT generation:
control-register changes, reset, and state application invalidate blocks and
watches. Epoch-only cache/TLB revalidation does not change this bit. The optional
change is based on that implementation invariant, not a general permission to
ignore writes whenever a CPU has an instruction cache.

The patch applies to the final default reviewed source. Its metadata test passed
**954/954**, seed `cd3018d4`, under GCC UBSan and AArch64/QEMU. The strengthened
CPU-write fixture passed **90 cases, 0 failures** with the optional patch under
both environments. An exact AArch64 wrong-order mutant, returning before page
tracking, failed 257 metadata expectations, all I=1 (697/954 passed, exit 1).
The 90 functional scenarios alone also pass that mutant: separate block
publication metadata can mask the misplaced return. The metadata oracle is the
test that directly demonstrates coverage of its placement.

### Final Release comparison

The final [C driver](../../../scripts/bench_code_revalidation.c) executes
synthetic RAM blocks containing `ADD r0,r0,#1` followed by `MOV pc,r1`, located
4 KiB apart. It warms each block twice and requires all requested native entries
to be compiled before timing. An optional internal cache-epoch invalidation
before each block isolates revalidation cost; it does not execute a real guest
MCR or reproduce a game's maintenance frequency. Time includes dispatch and
the guest arithmetic, not just the watch helper.

The driver checks retired instruction counts, result register, PC, unchanged
JIT generation, and unchanged compiled-code size. Profiling-OFF output correctly
has zero runtime counters; `ready_native_blocks` confirms the warm native entries.
Separate profiling-ON runs check actual native/portable instruction counters.
Do not interpret the Release `native=0` column as either a failed native run or
an actual native instruction measurement.

The final runner uses seven samples of 1,000,000 blocks, a seeded shuffle of
condition order, and adjacent alternating AB/BA comparisons. GCC 13.3 used
`-O3 -DNDEBUG -std=c2x -DGP32EMU_CPU_PROFILE=0`; Clang 18.1.3 used the same flags
with `-std=c23`. Every one of the 840 retained Release/control rows has `ok=1`.
The baseline is the published preceding review tree at
`f12176964c1979da105791f342ce0f5828176b3d` (local equivalent `5db6819`); the
candidate adds only the small I=1 early-return change to that CPU source.
It does not include the range-loop experiment.

| I=1, native revalidation | GCC before → after, ns/block | Clang before → after, ns/block |
|---|---:|---:|
| 48 instructions, 1 block | 98.751 → 62.360 (−36.85%) | 96.480 → 61.913 (−35.83%) |
| 48 instructions, 64 blocks | 111.263 → 73.417 (−34.01%) | 111.437 → 73.898 (−33.69%) |
| 96 instructions, 1 block | 177.786 → 106.111 (−40.32%) | 172.414 → 102.574 (−40.51%) |
| 96 instructions, 64 blocks | 189.235 → 115.823 (−38.79%) | 192.126 → 116.587 (−39.32%) |

Small no-maintenance controls were mixed: GCC 2-instruction/64-block was
21.269 → 21.903 ns (+2.98%), Clang 2-instruction/1-block was 11.307 → 11.748 ns
(+3.90%), while some longer Clang controls improved. I=0 native controls ranged
from −1.25% to +2.35% with GCC and −4.27% to +1.73% with Clang. Long portable
I=1 revalidation improved approximately 11–14.5%, but this is not enough to
infer whole-game behavior. Default adoption remains deferred because actual
game revalidation frequency and physical low-end AArch64 performance are unknown.

The final four timing windows, recorded in the JSON epoch fields, were:

| File | UTC window | Duration |
|---|---|---:|
| GCC I=1 | Oct 17 23:57:54.531–23:58:34.365 | 39.83 s |
| Clang I=1 | Oct 17 23:59:05.147–23:59:47.537 | 42.39 s |
| GCC I=0 | Oct 18 00:00:34.924–00:01:02.336 | 27.41 s |
| Clang I=0 | Oct 18 00:01:42.204–00:02:11.711 | 29.51 s |

### Repeating the optional comparison

Run from a checkout containing this report and the retained drivers. The source
selection macro lets the latest driver compare the preceding public CPU source
and just the CPU hunk of the optional patch. Use separate temporary worktrees;
do not stack both candidates.

```sh
review_root="$PWD"
measure_dir=$(mktemp -d /tmp/gp32-watch-review.XXXXXX)
git fetch origin codex/1.1.0-core-review
git worktree add --detach "$measure_dir/before" f12176964c1979da105791f342ce0f5828176b3d
git worktree add --detach "$measure_dir/after" f12176964c1979da105791f342ce0f5828176b3d
git -C "$measure_dir/after" apply --include=src/arm920t.c \
  "$review_root/docs/benchmarks/1.1.0-deep/candidates/skip-unused-word-watches.patch"
cc -std=c2x -O3 -DNDEBUG -DGP32EMU_CPU_PROFILE=0 \
  "-DCORE_UNDER_TEST=\"$measure_dir/before/src/arm920t.c\"" -Isrc -Iinclude \
  scripts/bench_code_revalidation.c -o "$measure_dir/proof-before"
cc -std=c2x -O3 -DNDEBUG -DGP32EMU_CPU_PROFILE=0 \
  "-DCORE_UNDER_TEST=\"$measure_dir/after/src/arm920t.c\"" -Isrc -Iinclude \
  scripts/bench_code_revalidation.c -o "$measure_dir/proof-after"
python3 scripts/bench_code_revalidation.py --before "$measure_dir/proof-before" \
  --after "$measure_dir/proof-after" --output "$measure_dir/i1.json" \
  --label 'record compiler/version/flags here; profile=0 I=1' --scope release --icache 1
python3 scripts/bench_code_revalidation.py --before "$measure_dir/proof-before" \
  --after "$measure_dir/proof-after" --output "$measure_dir/i0.json" \
  --label 'record compiler/version/flags here; profile=0 I=0' --scope controls --icache 0
```

For Clang, substitute `clang-18 -std=c23` in both compilations. The runner uses
the first allowed Linux CPU unless `--cpu` is provided. Record the platform,
compiler, source hashes, clock configuration and raw data for any device rerun.
The retained figures establish targeted opportunities and their observed costs;
they do not choose the winning default for every game and host.
