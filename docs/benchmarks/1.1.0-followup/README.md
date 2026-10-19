# 1.1.0 follow-up: raw measurements

These are synthetic host measurements, not gameplay FPS or handheld benchmarks.
The Korean [follow-up report](../../REVIEW-1.1.0-FOLLOWUP.ko.md) explains the
changes and their limitations. Each experiment isolates one change; the numbers
must not be added together as the speed of the combined final core.

## Environment

- Linux x86-64, kernel `6.18.55-x86_64`, glibc 2.39.
- Reported CPU: AMD EPYC 9V74 80-Core Processor. This task's allowed affinity
  set was CPUs 0–8; these are not measurements from a dedicated handheld.
- GCC 13.3.0 and Clang 18.1.3.
- Audio: `-O3 -DNDEBUG`, `-std=c2x` for GCC and `-std=c23` for Clang.
- CPU store permission: GCC `-O3 -std=c2x`, CPU profiling enabled to verify
  actual native/decoded execution counts.
- CPU 8 pinned for audio, CPU 0 pinned for the isolated write-permission run.
  Baseline and candidate used identical compiler options.

## Audio SSE2

Files: [GCC CSV](audio-sse2-gcc.csv), [Clang CSV](audio-sse2-clang.csv).
Each has 210 data rows: 7 rounds × 3 source rates × 5 input block sizes ×
2 variants. Destination is 44,100 Hz. Each timed measurement emits approximately
500,000 stereo output frames. `ns_per_frame` is elapsed monotonic time divided
by the actual output frame count. A frame contains both channels.

The driver prepares the coefficient table and processes a warmup block before
starting the clock. It alternates adjacent baseline/candidate order each round.
For each rate/block condition, take the median of the 7 values for each variant,
then compute `100 * (1 - candidate_median / baseline_median)`.
Shared-host scheduling/frequency effects remain; raw variation is retained.

`checksum` is a sum of selected output samples used by the timing loop. It is
not a hash of every output byte. The separate startup check compares all PCM
bytes, output counts, and initialized state after each of 1,024 changing stream
blocks, covering 278,448 emitted stereo PCM frames. All corresponding timing
rows also have matching frame counts and checksums.

The original resampler source is identical at main `1090ff1` and the prior
review baseline (remote `43cdc06`, local `86eb4f9`). Source SHA-256:

```text
baseline: e7dec79437189e4ec5a2fea254485884322fa87f356f36520aeeb5128f6976f8
final:    913b01cd38cf1ade9ce0be4cd456782a7eb2518f615058887dff551be7199a4e
```

The measured source/history SSE2 prototype and final production kernel have
identical GCC 13 x64 `.text` bytes (SHA-256
`2011cd0ffc20fdb31a94b7c98c10f36728525f79c75e9845d4fa331694f28992`).
The original and final AArch64 GCC `.text` are also identical (SHA-256
`573e7914502b826eca5316646fec501a5d157645de3b03734ef57ab4fc1c3565`).
The Clang 18 x64 measured prototype and final production `.text` were also
compared after integration and are identical (10,036 bytes, SHA-256
`26d1804f6b7e4151b82d327386fcbc72616b5d3f2ae16034ae4bae318103318f`).
No AArch64 performance is claimed.

From the repository root, where the baseline commit is available:

```bash
taskset -c 8 python3 scripts/bench_audio_resampler.py \
  --baseline 1090ff174459c57d42106a028cf4e16b2f5fe618 \
  --cc gcc --cflags='-std=c2x -O3 -DNDEBUG' \
  --rounds 7 --frames 500000 > audio-gcc.csv

taskset -c 8 python3 scripts/bench_audio_resampler.py \
  --baseline 1090ff174459c57d42106a028cf4e16b2f5fe618 \
  --cc clang-18 --cflags='-std=c23 -O3 -DNDEBUG' \
  --rounds 7 --frames 500000 > audio-clang.csv

python3 scripts/bench_audio_resampler.py \
  --baseline 1090ff174459c57d42106a028cf4e16b2f5fe618 \
  --cc aarch64-linux-gnu-gcc --cflags='-std=c2x -O3 -DNDEBUG' \
  --runner='qemu-aarch64 -L /usr/aarch64-linux-gnu' --rounds 0
```

Use an allowed CPU on the machine being measured. The comparator requires the
two revisions to have the same resampler state layout, and its driver uses a
POSIX monotonic clock. QEMU is used for correctness only. Windows/MSVC and
32-bit x86 execution were not part of these results.

## Isolated cached-write permission change

[write-permission-isolated.json](write-permission-isolated.json) retains all
16 samples in execution order. This compares prior-review baseline `86eb4f9`
(same tree as remote `43cdc06`) against only the write-permission fix, local
commit `da3645d` (the same file tree as published commit
[`586862f`](https://github.com/gajae1/gp32emu/commit/586862f6a62e49afd40006a25211075cb0b64fd3)).
It does not include subsequent CPU code-write watches.

The loop mixes STR, STRB, STRH, STM, ADD and B. After 600,000 warmup guest
instructions it retires 60,000,000 measured guest instructions, in 1,000 calls
of 60,000. Four samples per version/mode use ABBA ordering, repeated twice.
The final register/memory values and actual backend counts match throughout.
Median elapsed times:

| Execution mode | Before | After | Interpretation |
|---|---:|---:|---|
| Native | 80.414 ms | 80.243 ms | Essentially unchanged in this loop |
| Decoded portable | 530.322 ms | 485.306 ms | 8.49% lower in this loop; code-layout sensitive |

The improvement is not an instruction-count proof or a game-speed estimate.
The production change caches a separate write base instead of authorizing every
store through a new callback.

The [driver](../../../scripts/bench_write_permission.c) can include another
checkout's source through `BENCH_ARM_IMPL`:

```bash
cc -std=c2x -O3 -Isrc -Iinclude \
  scripts/bench_write_permission.c -o /tmp/gp32-write-after
cc -std=c2x -O3 -Isrc -Iinclude \
  -DBENCH_ARM_IMPL='"/absolute/path/to/baseline/src/arm920t.c"' \
  scripts/bench_write_permission.c -o /tmp/gp32-write-before
taskset -c 0 /tmp/gp32-write-before 1
taskset -c 0 /tmp/gp32-write-after 1
```

Select the revision containing only the permission fix to reproduce the isolated
comparison. Building the current combined core measures its additional changes
as well. The output columns are mode, nanoseconds, r0, stored word, actual native
instruction count, and decoded-portable instruction count.

## CPU self-modifying-code watches

The separate [CPU code-write report](../../CPU-CODE-WRITES.md) and
[360-sample comparison](../../measurements/cpu-code-write-words-x64.json) record
baseline, rejected 64-byte watches, and the sound 4-byte-word candidate.
These measurements start from the prior-review baseline and isolate SMC,
without the independently cached write-permission fix.
The [earlier 64-byte run](../../measurements/cpu-code-writes-x64.json) is retained
as rejected-prototype history, not a second final performance claim.

The watch-table allocation is 262,148 bytes (256 KiB plus a 4-byte padding word).
In the measured x64 profiling build, the CPU struct grew from 347,840 to
609,992 bytes including ABI alignment. The independently appended writable
RAM pointer in the combined core adds another pointer-sized field.
These are object/allocation sizes, not a measurement of device RSS.
