# Regression and final-validation evidence

The files here preserve the actual focused failure output and final validation
logs from the deep review. They use synthetic inputs, with no game or BIOS
images. See [the report](../../../REVIEW-1.1.0-DEEP.ko.md) for interpretation and
[the measurement protocol](../README.md) for timing data.

The tested default source is local `a3e7d248814753edb8ec5ba454e32f730b19c019`,
equivalent to public `252db3e1863035b407783021d611d1bb3c521886`:
tree `b93fd183586c0c129ef917a5c91f3f262bcc752c`. Subsequent documentation commits
do not change that source. [validation.json](validation.json) records source
SHA-256 values, compiler settings, results, and hashes of the copied logs.
[commit-map.json](commit-map.json) maps each local source commit to its public
equivalent. Every published tree was checked for equality; different commit
metadata gives the two histories different commit IDs.

## Before/after regressions

| Files | Meaning |
|---|---|
| [thumb-transfer-before.log](thumb-transfer-before.log) | New transfer fixture against the preceding unmodified core: 1,030 cases, 6,453 Thumb assertions fail, zero ARM STMDB control failures. The fixture prints only its first 20 failures plus total counters. |
| [irq-latch-before.log](irq-latch-before.log), [after](irq-latch-after.log) | Selected IRQ changes and acknowledgement regression: 16 pre-fix assertion failures; corrected fixture passes. |
| [legacy-irq-before.log](legacy-irq-before.log), [after](legacy-irq-after.log) | Legacy multi-bit selection fixture against latch-only versus normalized load: six pre-normalization assertion failures; corrected fixture passes. |
| [legacy-irq-probe.c](legacy-irq-probe.c), [after output](legacy-irq-probe-after.log) | Separate public-API probe that writes a real old-core state and acknowledges the stored selection after loading it. |
| [smartmedia-capacity-before.log](smartmedia-capacity-before.log), [after](smartmedia-capacity-after.log) | Supported 128 MiB data cards including spare bytes: 20 pre-fix assertions fail; corrected fixture passes. |

The retained tests are in `tests/`. To repeat a red/green comparison, build the
same new fixture against the parent and child of its named fix, with matching
compiler options. The Thumb transfer fixture's ARM control is intentionally
raw. The SmartMedia fixture allocates several hundred MiB for large synthetic
cards and full-state fallback; it is not a low-memory-device benchmark.

The legacy probe is supplemental evidence, not a new installed executable or
CTest target. Build the same probe against an old library and a corrected one:

```sh
cc -std=c2x -O2 -Iinclude -Isrc \
  docs/benchmarks/1.1.0-deep/evidence/legacy-irq-probe.c \
  /path/to/old/libgp32emu.a -lm -pthread -o /tmp/irq-old
cc -std=c2x -O2 -Iinclude -Isrc \
  docs/benchmarks/1.1.0-deep/evidence/legacy-irq-probe.c \
  /path/to/fixed/libgp32emu.a -lm -pthread -o /tmp/irq-fixed
/tmp/irq-old write /tmp/irq-legacy.state
/tmp/irq-fixed read /tmp/irq-legacy.state
```

The original writer used the SoC source from local `22a4e7e` (public equivalent
`b73bc22`). It serialized `SRCPND=INTPND=0x00000c00`, `INTOFFSET=10`. A latch-only
reader leaves offset 10 after acknowledging source 10, even though only source
11 remains. The normalized reader chooses 10, then 11, then clears the line.
Use this probe with its generated trusted fixture; it is not a general-purpose
state-file inspection tool.

## Final default matrix

The four CMake builds all require C23 and enable tests, libretro, and CPU
profiling. All completed successfully. `ctest -j2 --output-on-failure` then
reported 49/49 for each build:

| Log | Configuration | Elapsed test time |
|---|---|---:|
| [GCC](ctest-gcc.log) | GCC 13.3 x86-64 Release, `-std=c2x` | 7.59 s |
| [Clang](ctest-clang.log) | Clang 18.1.3 x86-64 Release, `-std=c23` | 6.88 s |
| [AArch64/QEMU](ctest-aarch64-qemu.log) | AArch64 GCC 13.3 Release, QEMU with `/usr/aarch64-linux-gnu` | 60.31 s |
| [UBSan](ctest-ubsan.log) | GCC 13.3 RelWithDebInfo, undefined sanitizer with recovery disabled | 8.81 s |

These elapsed times are validation records, not a performance comparison across
compilers. Existing compiler warnings remain; no warning-free build is claimed.

[WASM output](wasm-runtime.log) records the final Clang/LLD 18 build and Node
runtime checks. The repeated-save runner performs 32 saves of 9,290,562 bytes;
the allocator heap-used metric stays at 38,340,656 bytes after staging. This is
not browser RSS or a physical audio-device test. The clock fixture verifies the
host monotonic bridge; it does not establish a new browser autosave UI feature.

## Optional patch, separate from the default matrix

The retained I=1 word-watch omission patch was applied in an isolated worktree.
AArch64 GCC 13.3 (`-std=c2x -O1 -g`) with QEMU 8.2.2 produced:

- [CPU code writes](optional-aarch64-cpu.log): 90 cases, zero failures, exit 0.
- [Metadata oracle](optional-aarch64-tracking.log): 954/954, seed `cd3018d4`, exit 0.
- [Wrong-order mutant](optional-aarch64-wrong-order.log): 697/954 passed, 257
  mismatches with I=1, same final seed, exit 1. Its only change moved the I=1
  early return above unconditional page tracking.

The 90 CPU scenarios alone do not reject that misplaced-return mutant; the
metadata oracle detects the missing page updates. These optional-patch results
are not a claim that a 50th default CTest ran, that the optimization is enabled,
or that it improves physical AArch64 performance.
