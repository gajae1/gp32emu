# Round 5: reproducible core review evidence

The [Korean review](../../REVIEW-1.1.0-ROUND5.ko.md) explains the accepted
correctness fixes, component performance tradeoffs, rejected native predicate
experiments, state compatibility, and next validation priorities.

Before is public `2bace9afd9c3d1e5e7cac9d23379e2f60d167fec`, tree-identical to
local `85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f`. Integrated production/tests
are frozen at local `865f88e2d095f71ac80935bd775a38f3b8985246`, tree
`e300f1f9b419f24da5f96b1e831640e47a9a32ce`. Subsequent commits retain those
source bytes and add review/evidence. Final publication/export manifests map
the delivered tree and artifacts to that freeze. Local worktree commit IDs in
historical logs are not all public Git references; the optional reproduction
history bundle retains the isolated audit branches and their objects.

## Evidence map

| Directory | Contents and units |
| --- | --- |
| [writes](writes/README.md) | Adopted external-watch/string changes and scalar notification costs. Both compilers, 1,008 timed samples, all 72 condition summaries, exact binaries/source/build logs, coverage-only runs and a validated table renderer. |
| [jit](jit/README.md) | Native complete-range certificate and code-write ordering, independent predicate/helper-join tests, and two **rejected** predicate candidates with all 2,128 timed samples. |
| [interpreter](interpreter/README.md) | 2,478 direct-mapping cases; 872,064 independent multiply cases; 10,596 rare-region cases; exact isolated-before reconstruction and retained fixture mistakes. |
| [hle](hle/README.md) | Fourteen HLE writer gateways, 155 total cases, 60 original assertion failures; external 12,920-case word-watch oracle, 4,356 original false-positive epochs. |
| [bios](bios/README.md) | Public reload, staged failed file reads and overlapping input. Separate 12-case / 12-check fixtures, before/after logs and source hashes. |
| [bounds](bounds/README.md) | 199 direct-memory/certificate checks including huge `size_t`, copied-bus RAM growth and permission boundaries; 18 before assertions fail. |
| [pwm](pwm/README.md) | Hardware-source analysis, 50 scenarios/3,789 model checks, chronological DMA/IRQ behavior, 105 PWM state scenarios and 62 machine-speed-state cases. Explicit v16 migration policies and remaining model limits. |
| [audio](audio/) | GCC/Clang object disassembly showing elimination of the apparent large resampler copy; no runtime change or benchmark claim. |
| [verification](verification/README.md) | Four fresh 64/64 C23 matrices, 82 actual compile commands and 194 frozen input hashes per matrix, retained warnings and independent integration review. |
| [asan](asan/README.md) | Fresh focused five-test ASan run, first attempt, sequential, no diagnostics; leak detection explicitly disabled. |
| [wasm](wasm/README.md) | Fresh wasm32 C23 build, original save-only gate, both modules' 32 public load/save round trips, exact 92-byte PWM tail and bounded allocator-placement diagnosis. |
| [baseline](baseline/baseline.json) | Reused Round4 GCC 54/54 rerun, labeled as reused rather than a fresh baseline build or timing comparison. |

## Reading the counts

Test scenarios, failed assertions, emitted/retired native instructions, and
performance samples are different units. Do not add them together as a bug
count. A retained failure may also be a test-harness assumption, setup error,
or deliberate rejected candidate; each report identifies which it is.

The final integrated inventories contain 64 tests each and all pass. The
2,504 PWM before failures use the same 49 ordinary scenarios, excluding the
new billion-iteration avoidance stress on the old implementation. The rare
region final isolated before has 4,026 failures, of which 3,580 are conservative
interval notifications; the separate public-dispatch BL cases expose actual
stale execution. The older 4,246 comparison also lacked the RAM refinement and
is retained as a historical comparison, not the isolated claim.

The first predicate study did not alternate before/after within each case as
originally intended; its JSON and report now state the actual order. The
smaller follow-up and both write studies verify per-case AB/BA alternation.
All samples remain, including slower controls. The original and corrected
runner versions are available. No candidate was selected by deleting an
unfavorable result.

## Scope of performance conclusions

All **3,136 new timed samples** use a pinned shared x86-64 host and synthetic
workloads with profiling disabled. Positive execution coverage is collected
separately. They establish bounded component effects and explicit costs, not
game FPS, battery life, physical AArch64 speed or a whole-program weighted
improvement. Correctness matrices and QEMU wall times are not benchmarks.

The word-watch refinement is the external-notification consumer of the
existing instruction bitmap. It does not adopt the older CPU word-watch
tracking-loop candidate, and it requires I=1 word watches. The old
`skip-unused-word-watches.patch` is now marked incompatible in both prior
documents. Rejected native predicate patches in this directory also remain
research artifacts; the final core keeps the prior predicate emitter.

## Practical reproduction

Use a clean delivered checkout and a new build directory. The final
`scripts/verify_core_round5.py` accepts the checkout's own HEAD as its
`--frozen-head`, validates all source hashes and compile flags, and requires
the 64-test inventory. See [the exact invocation](verification/README.md).
WASM can be rebuilt directly with the recorded Make command and the final
supplemental Node driver; the recorded Python wrapper also preserves the
original local freeze check. Component builders accept separate source
worktrees and output paths; their manifests retain all actual flags.

No release or merge is part of this continuation. The accompanying source,
patches and Draft PR carry the tested production bytes. The audit history is
provided for reconstruction; individual rejected intermediate commits should
not be used as a release state.
