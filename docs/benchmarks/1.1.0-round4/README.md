# Round-four measurements and reproducible evidence

Reviewed on 2026-10-18 UTC, starting at public commit
`b141220cd5fd9d628305133fc963ed86a30f7e41`.
The [Korean engineering report](../../REVIEW-1.1.0-ROUND4.ko.md) explains the
changes, remaining model gaps, interpretation, and final integration results.

## Adopted performance changes

| Study | Timed samples | Decision and qualifications |
|---|---:|---|
| [x64 in-block forward branches](x64-forward.md) | 476 | All 126 taken-path pairs improved. Untaken and repeated code-generation costs are retained. No physical ARM or game claim. |
| [Thumb LDMIA/STMIA selected-register iteration](README-thumb-transfers.md) | 336 | All 12 Thumb medians improved; one paired outlier and changed raw-ARM controls remain in the data. GCC/Clang scope. |
| [HLE bounded bulk zeroing and invalidation](README-hle-zero.md) | 504 | Large repeated clears improved; four-byte calls got slower. Includes the required notification omitted by the incorrect baseline. |

There are **1,316 timed samples**, plus 102 separate clock-suppressed x64
verification samples. These are three isolated comparisons, not measurements
of the combined emulator's aggregate speed. The percentages are not additive.
Timing uses profiling OFF; native execution evidence uses profiling ON.

All paired timing windows paused other agents' builds, tests, QEMU and
measurements. Shared-host scheduling and frequency remain outside that control.
No ROM, BIOS image, private game corpus or connected device is included.

## Exact source reconstruction

The [isolation directory](isolation/) contains source-only before/after patches
for all three studies. Each pair starts independently from the public base.
The [reconstruction proof](evidence/isolation-reconstruction.json) verifies
all six measured source hashes. These are historical experiment sources;
do not apply those patches to the current review branch as extra optimizations.

The final combined code has its own source/tree identities and functional
matrix in [validation.json](evidence/validation.json). A measured isolated
file is intentionally different from the final file with independent fixes.
The [commit map](evidence/commit-map.json) records equal-tree local/public
identities for review and reconstruction.

## Optional experiment and unresolved behavior

- [A64 second-way chaining](optional-a64-chain.md): seven cold instructions,
  30 guard/budget scenarios and a full native differential run; retained as an
  optional patch because physical first-way/second-way/miss costs are unknown.
- [PWM model-gap probe](evidence/pwm-model-gap.c) and
  [observations](evidence/pwm-model-gap.log): 25 mismatches concerning active
  count versus buffered reload/manual update/stop. Intentionally outside
  passing CTest; the model and saved-state extension remain future work.

## Functional evidence

The [evidence index](evidence/README.md) maps baseline failures to focused
tests and distinguishes assertions from scenarios. It also documents the
first integration attempt, the fixture corrections, and the discarded stale
GCC test run. Final compiler, build, full-test and WASM logs are retained with
source hashes and reproducible commands.

The [Switch workflow check](evidence/switch-ci/README.md) separately records the
duplicate-core link failure and the corrected full differential in both Linux
mapping mocks. It does not claim a physical Switch or libnx/NRO build.

## Artifact integrity

[SHA256SUMS.txt](SHA256SUMS.txt) covers the files in this directory and the main
round-four report, excluding the checksum file itself. Run the following from
this directory to verify the retained bytes:

```sh
sha256sum -c SHA256SUMS.txt
```

Earlier results remain under [1.1.0-deep](../1.1.0-deep/),
[1.1.0-followup](../1.1.0-followup/), and
[CPU code-write measurements](../../CPU-CODE-WRITES.md). Their original
workload, compiler and source scopes still apply.
