# Thumb LDMIA/STMIA register-list iteration

This isolated experiment, run on 2026-10-18 UTC, compares the existing eight-bit
scan with iteration over only the selected bits. The candidate changes the two
Thumb LDMIA/STMIA loops in `src/arm920t.c` to use the existing `arm_ctz16` helper.
It retains the original list for writeback decisions and preserves all address,
register, callback, and instruction-boundary behavior. PUSH, POP and the raw
ARM transfer implementation are unchanged.

The change is adopted on the strength of the defined-behavior regression and
the bounded measurements below. These are synthetic x86-64 host measurements,
not game FPS or physical ARM device measurements.

## Functional coverage

The independent oracle in
[`arm_thumb_transfer_test.c`](../../../tests/arm_thumb_transfer_test.c) now runs
**14,278 cases**. It retains the existing 1,030 PUSH/raw ARM controls and adds
5,086 STMIA and 8,162 LDMIA cases. The added cases cover every nonempty defined
list and all eight base registers at all four address alignments, plus IRQ and
stop requests from the first bus callback. They check transfer addresses and
values, final registers and flags, base writeback, and completion before the
next instruction/IRQ boundary. Both old and new loops pass; no correctness
defect is claimed for these defined transfer cases. GCC 13.3 and Clang 18.1.3
both pass the final regression.

The oracle excludes the non-first STM base value, which the generic ARM
architecture specifies as UNPREDICTABLE. Ambiguous Samsung manual prose is not
used to change that value. Thumb LDM retaining a loaded base and STM storing
an original first base are defined in
[ARM ARM DDI0100I](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a)
A7-44/45 and A7-96/97.

## Measurement method

The [C driver](../../../scripts/bench_thumb_transfers.c) executes a fixed
five-instruction loop: restore the base from SP, STMIA, restore the base,
LDMIA, and branch back. Thumb uses r0 as the base, so a selected base is always
the first register. The unchanged raw ARM controls use r12, outside every
selected list; they do not exercise ARM LDM writeback/base aliases.

Each sample performs 30,000 warmup instructions, then measures 3,000,000 guest
instructions with `CLOCK_MONOTONIC`. The exact registers, memory words, flags,
PC and retired count are checked outside the timed region, and every sample
retains a matching state hash. No external ROM, game or device workload is
used. The [Python driver](../../../scripts/bench_thumb_transfers.py) pins to
permitted CPU 0 and alternates AB/BA order for seven repetitions.

The shared x86-64 Linux 6.18.55 environment reported an AMD EPYC 9V74 CPU.
Other agents paused compilation, functional tests and performance runs during
the coordinated window. This does not remove host-level scheduling or
frequency noise. An initial parser failure, after one checked sample, was
corrected before restarting the complete matrices; that incomplete sample is
not part of the retained results.

The two JSON files retain **336 samples**: 2 compilers × 2 instruction sets ×
6 lists × 7 repetitions × 2 variants. Both builds use `-O3 -DNDEBUG` and
`-DGP32EMU_CPU_PROFILE=0`, with `-std=c2x` for GCC 13.3.0
(`6ubuntu2~24.04.1`) and `-std=c23` for Clang 18.1.3 (`1ubuntu1`).

## Results

Numbers are median host nanoseconds per guest instruction, including the
base-restoration and loop instructions. Negative changes mean less host time.

| Thumb register mask | GCC before | GCC after | Change | Clang before | Clang after | Change |
|---|---:|---:|---:|---:|---:|---:|
| `01` | 8.5933 | 6.8370 | -20.44% | 9.0935 | 7.4179 | -18.43% |
| `80` | 8.2758 | 6.8439 | -17.30% | 8.7172 | 7.4198 | -14.88% |
| `81` | 9.2109 | 7.8735 | -14.52% | 9.6933 | 8.4676 | -12.64% |
| `55` | 11.1394 | 9.9150 | -10.99% | 11.5983 | 10.5103 | -9.38% |
| `7f` | 14.3530 | 12.9762 | -9.59% | 14.5061 | 13.7225 | -5.40% |
| `ff` | 15.4295 | 14.0816 | -8.74% | 15.4940 | 14.7666 | -4.69% |

All 12 Thumb medians improved. Of 84 paired Thumb comparisons, 83 improved;
one GCC full-list pair changed by +0.82%. Its seven after samples ranged from
13.7490 to 15.7374 ns/instruction, while its median was 14.0816. Raw samples
remain available rather than removing the outlier.

The unchanged raw ARM controls are material to interpretation. Their GCC
medians changed by -0.98% to +1.28%; their Clang medians changed by -7.23% to
-2.30%. Some raw pairs also contain scheduling outliers. These controls show
that the comparison does not isolate the cost of a bit-scan instruction:
compiler decisions, code layout and host noise can affect unchanged paths.
The ARM control movement is not an ARM transfer optimization claim.

## Provenance and reproduction

The baseline is local experiment commit `7dd1290`, with the ARMv4T control
whitelist fix already applied. Only the two Thumb transfer loops differ in
the candidate core. Integration commits may use different IDs; these hashes
identify the exact measured source bytes.

| Item | SHA-256 |
|---|---|
| Baseline `src/arm920t.c` | `3f2fff84a5de7e53ee5f02319f3f54485fcf0e9188de3a55e698d3d4d9b51324` |
| Candidate `src/arm920t.c` | `9b6c89ddd91e5261bc1a47cd3ae8b8d1d652c3ed70bcdc1dcc330ba15344a656` |
| C driver | `3637f421c1a43461fad1b92cd9a62695bc43d74f68bc5da7e87618ea203d4567` |

For each compiler, link the same C driver against the before and after core
source files using the flags above and `-Iinclude -Isrc`. Run, for example:

```sh
python3 scripts/bench_thumb_transfers.py /path/to/before /path/to/after results.json \
  --instructions 3000000 --repetitions 7 \
  --configuration 'record compiler package, exact flags, profiling and source hashes'
```

[`thumb-transfer-gcc.json`](thumb-transfer-gcc.json) and
[`thumb-transfer-clang.json`](thumb-transfer-clang.json) include every checked
output, the actual binary paths, affinity, configuration and grouped medians.
Rebuilding on another host reproduces the method rather than the historical
timings. No claim is made about workload frequency, whole-emulator speed or
the magnitude on AArch64 hardware.
