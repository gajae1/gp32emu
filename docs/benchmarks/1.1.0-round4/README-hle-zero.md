# Direct-HLE RAM zeroing: bounds, code coherence and measured cost

The adopted change uses the SoC bus's complete writable RAM span for a bulk
`memset`, then notifies the CPU that the destination may contain decoded code.
The old fallback issued one SoC byte write per byte and gave no notification.
The same bounds helper rejects an incomplete or wrapping range before a write.

## Correctness

[`gp32_hle_memory_test.c`](../../../tests/gp32_hle_memory_test.c) contains
71 scenarios: 60 valid length/placement combinations, five rejected ranges,
and six code-overwrite cases across raw ARM, cached ARM and requested native
execution with the guest I bit both clear and set. The code cases set CP15
through a real MCR, execute an old MOV, zero that instruction, and execute
the result. Four cached/native cases retained the old MOV before the fix.
The raw ARM controls saw the new zero word. All 71 pass after the change.

The test compares all 8 MiB of RAM for its range checks, including the last
valid byte and zero-length requests. The code checks also verify registers,
PC, flags, retired instructions and cycles. Native priming is checked when
the profiling build exposes a supported native backend.

## Method

The [C driver](../../../scripts/bench_hle_zero.c) creates a normal GP32 core
and calls this helper directly; no guest CPU instructions or peripherals run
inside the timer. Each sample executes 64 warmup operations, then repeatedly
clears either one span or eight rotating spans. All other bytes, including
64-byte guards, are verified across the entire 8 MiB allocation afterwards.
Buffers become zero during warmup, so this is a repeated, resident-memory
helper benchmark. It does not model cold game startup or whole-emulator FPS.

Each condition processes a target 64 MiB, bounded to 256..4,000,000 calls.
There are nine sizes, two working sets and seven alternating AB/BA rounds.
The [Python driver](../../../scripts/bench_hle_zero.py) pins to permitted CPU 0.
Each compiler contributes 252 samples; the complete comparison retains
**504 samples**, including slower results. Other agents paused all compilation,
tests, QEMU and timing work during the window. Host scheduling/frequency noise
outside this workspace remains possible.

| Setting | Recorded value |
|---|---|
| Host | x86-64 Linux 6.18.55, glibc 2.39, shared AMD EPYC 9V74 host |
| GCC | 13.3.0, package `13.3.0-6ubuntu2~24.04.1`, `-O3 -DNDEBUG -std=c2x` |
| Clang | 18.1.3, `-O3 -DNDEBUG -std=c23` |
| Profiling | `GP32EMU_CPU_PROFILE=0` for the driver and linked core |
| Other core objects | Identical before/after within each compiler; no IPO/LTO |
| GCC window | 2026-10-18 10:38:34.232786–10:38:56.397031 UTC |
| Clang window | 2026-10-18 10:38:56.473383–10:39:18.373841 UTC |

## Results

Medians below are host nanoseconds per helper call, using one repeated span.
Both working sets and every raw sample are in the JSON files.

| Bytes | GCC before | GCC after | Time change | Clang before | Clang after | Time change |
|---|---:|---:|---:|---:|---:|---:|
| 4 | 9.481 | 10.733 | +13.20% | 8.365 | 11.588 | +38.53% |
| 8 | 16.321 | 10.722 | -34.31% | 14.642 | 11.324 | -22.66% |
| 16 | 29.809 | 10.493 | -64.80% | 27.894 | 11.322 | -59.41% |
| 64 | 111.233 | 10.241 | -90.79% | 108.205 | 11.053 | -89.79% |
| 256 | 435.101 | 10.518 | -97.58% | 426.957 | 11.671 | -97.27% |
| 4,096 | 6,857.262 | 48.633 | -99.29% | 6,775.950 | 48.733 | -99.28% |
| 65,536 | 109,978.427 | 601.125 | -99.45% | 108,660.097 | 602.982 | -99.45% |
| 153,600 | 254,779.236 | 1,385.193 | -99.46% | 255,094.679 | 1,359.397 | -99.47% |
| 524,288 | 880,173.445 | 4,909.582 | -99.44% | 874,003.203 | 4,867.562 | -99.44% |

The four-byte case gets slower in both working sets: approximately 1.1–3.2 ns,
or +11.4..38.5%. The comparison includes the necessary code-invalidation
notification, which the incorrect baseline omitted. Large clears remove a
byte-dispatch loop and benefit substantially. No tiny-size threshold was
fitted to these measurements. A native device experiment should preserve the
notification and measure its own size distribution before selecting one.

These percentages are not additive with DMA, FIR or Thumb improvements.
Call frequency is unknown without a game workload. Large speedups in a rarely
called initialization helper can have little effect on steady-state emulation.

## Exact source and reconstruction

| Source | SHA-256 |
|---|---|
| Baseline `src/gp32.c` | `58e33301584e9e21388cf2a8f56a7e368e49b5726913561bb8fd0571eb9b22d9` |
| Candidate `src/gp32.c` | `e2d2857e7c827e75f4e8fe5ed65b1cddd3ab092cada5479949356b8064bc8a70` |

The baseline GP32 file is unchanged from public commit `b141220`. The other
core objects were built at local integration step `fa11e03` (the BIOS span,
DMA trigger and HLE zero changes, before later IRQ/fetch/state changes).
The two variants link the same other objects, so only the included GP32 file
differs. Later integration files have different hashes; their extra changes
were not part of this isolated comparison.

The source-only patches in [isolation](isolation/) reconstruct this study from
`b141220`: apply `hle-zero-support.patch`, preserve `src/gp32.c` as the before
file, then apply `hle-zero-candidate.patch` for the after file. Build the core
archive with profiling and IPO disabled. Compile the same C driver twice,
selecting each included GP32 file with `GP32_CORE_UNDER_TEST` and linking the
same archive plus `-lm -pthread`.

For example, the definition for an absolute source path is passed as
`-DGP32_CORE_UNDER_TEST='"/path/to/before.c"'`; the driver also needs
`-Iinclude -Isrc`. Use the flags in the table and separate GCC/Clang builds.
Run the paired binaries with:

```sh
python3 scripts/bench_hle_zero.py /path/to/before /path/to/after results.json
```

[`hle-zero-release-gcc.json`](hle-zero-release-gcc.json) and
[`hle-zero-release-clang.json`](hle-zero-release-clang.json) record the binary
hashes, affinity, sizes, iteration counts, checked output and raw elapsed times.
Rebuilding elsewhere reproduces the method, not the historical nanoseconds.
