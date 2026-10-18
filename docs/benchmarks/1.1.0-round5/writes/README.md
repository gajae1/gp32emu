# External code-write and HLE output measurements

The complete integrated Round 5 source was compared with the preceding public review.
This is a component study on one shared x86-64 host, not a game/FPS or physical handheld benchmark.

## Source and method

- Before: clean local `85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f`, tree-identical to public `2bace9afd9c3d1e5e7cac9d23379e2f60d167fec`.
- After: clean local source/test freeze `865f88e2d095f71ac80935bd775a38f3b8985246`. Later commits change documentation only.
- AMD EPYC 9V74, Linux 6.18.55, CPU affinity 0. GCC 13.3 and Clang 18.1.3; identical `-O3 -DNDEBUG -std=c2x -fno-lto` driver flags. CMake core libraries require C23 and disable IPO.
- Profiling is disabled in every timed binary. Separate profiling-enabled binaries pass 40 external and 32 HLE verification rows per compiler, with clocks suppressed; native rows require all retired instructions to be native.
- Every individual condition has seven before/after pairs, ordered AB, BA, AB, BA, AB, BA, AB. No outlier was removed. The two studies contain 560 external and 448 HLE timed samples: **1,008 total**.
- All builds, sanitizer tests and functional/WASM executions completed before the sequential measurement window. Agents continued lightweight documentation work. Other shared-host activity, frequency scaling and temperature were not controlled.
- Each sample excludes setup, 64 warmups and verification. The driver verifies registers, PC, CPSR, retired work and every byte of 8 MiB RAM outside its clock. The compiler barrier materializes repeated host stores; it is not a CPU memory fence.
- The tables show medians of seven samples in nanoseconds per call, their percentage change, the median of seven paired percentage changes, and the number of improving pairs. Negative change means less time.

The build manifests contain every compiler command, source and driver hash, matching core-library compilation, and executable hash. The eight `*-build.log` files match the manifest checksums. Timing JSON files retain every sample and pair; `measurement-window.json` records all four sequential commands. Its original context wording overstated the pause as all agent execution; documentation work continued. The context was corrected without changing commands, times or samples, and the exact original manifest and runner are retained as `measurement-window-original.json` and `run-measurements-original.py`. `driver-selfcheck-*` compares a baseline executable with itself and is only a harness self-check, not an optimization result.

These are **whole-source-snapshot comparisons**. Mapping correctness, rare-region tracking, direct-span bounds and compiler layout changes coexist with the external-watch refinement and string batching. No isolated percentage is assigned exclusively to one edited C function.

## External-write workload

Each timed iteration stores one data word, optionally notifies an external physical span, restores r0 and PC, and executes `MOV r0,#0x11; BX r12`. The native-disabled case uses the cached executor. MMU is disabled; CP15.I=0 and I=1 are separate cases. Every sample runs 250,000 iterations and retires exactly 500,000 guest instructions. The unit includes the store, notification, two public register updates and dispatch; it is not isolated notification latency.

| Scenario | Meaning |
| --- | --- |
| 0 | No-notification control; the data store still executes. |
| 1 | Four-byte data notification immediately after the two watched instructions, on the same page. |
| 2 | A 128-byte non-code span farther away on the same page; only its first word is written in this workload. |
| 3 | Four-byte data notification on another page. |
| 4 | Notification overlaps an unchanged instruction word, while the materialized data store is at code+8. This positive-hit control must still renew the epoch. |

Same-page data notifications avoid false-positive epoch changes after refinement. All 112 pairs in scenarios 1/2 improve; their median component time falls by about 27–34% for cached and 49–54% for native execution. Other-page notifications add about 1.1–2.2 ns, and actual watched-word notifications add about 2.3–4.7 ns. These costs are retained, not called noise. The old path remains correct in these particular data cases but performs unnecessary translation revalidation; this study does not imply compilation on every epoch change.

### External: gcc

| Executor | I | Scenario | Before ns | After ns | Change | Paired median | Improving pairs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Cached | 0 | 0 | 22.423 | 22.109 | -1.40% | +0.49% | 3/7 |
| Cached | 0 | 1 | 39.248 | 25.916 | -33.97% | -33.19% | 7/7 |
| Cached | 0 | 2 | 39.074 | 25.678 | -34.28% | -34.11% | 7/7 |
| Cached | 0 | 3 | 22.126 | 24.101 | +8.93% | +8.93% | 0/7 |
| Cached | 0 | 4 | 38.811 | 41.203 | +6.16% | +6.16% | 0/7 |
| Cached | 1 | 0 | 21.883 | 21.980 | +0.44% | +0.07% | 3/7 |
| Cached | 1 | 1 | 38.656 | 25.807 | -33.24% | -33.32% | 7/7 |
| Cached | 1 | 2 | 38.911 | 25.914 | -33.40% | -33.84% | 7/7 |
| Cached | 1 | 3 | 22.199 | 23.939 | +7.84% | +7.92% | 0/7 |
| Cached | 1 | 4 | 38.706 | 41.308 | +6.72% | +6.72% | 0/7 |
| Native | 0 | 0 | 10.238 | 10.038 | -1.95% | -1.89% | 7/7 |
| Native | 0 | 1 | 28.265 | 13.055 | -53.81% | -53.81% | 7/7 |
| Native | 0 | 2 | 27.723 | 13.076 | -52.83% | -52.80% | 7/7 |
| Native | 0 | 3 | 10.778 | 11.982 | +11.17% | +11.07% | 0/7 |
| Native | 0 | 4 | 27.897 | 30.164 | +8.13% | +8.18% | 0/7 |
| Native | 1 | 0 | 10.331 | 10.051 | -2.71% | -2.47% | 6/7 |
| Native | 1 | 1 | 27.845 | 13.069 | -53.06% | -53.21% | 7/7 |
| Native | 1 | 2 | 27.944 | 13.080 | -53.19% | -52.96% | 7/7 |
| Native | 1 | 3 | 10.866 | 11.976 | +10.21% | +10.91% | 0/7 |
| Native | 1 | 4 | 27.869 | 30.528 | +9.54% | +9.81% | 0/7 |

### External: clang

| Executor | I | Scenario | Before ns | After ns | Change | Paired median | Improving pairs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Cached | 0 | 0 | 26.432 | 25.880 | -2.09% | -2.08% | 5/7 |
| Cached | 0 | 1 | 41.143 | 29.868 | -27.40% | -27.96% | 7/7 |
| Cached | 0 | 2 | 41.156 | 29.892 | -27.37% | -27.24% | 7/7 |
| Cached | 0 | 3 | 26.683 | 28.866 | +8.18% | +8.60% | 0/7 |
| Cached | 0 | 4 | 41.755 | 45.980 | +10.12% | +9.75% | 0/7 |
| Cached | 1 | 0 | 26.747 | 25.855 | -3.34% | -3.19% | 7/7 |
| Cached | 1 | 1 | 41.616 | 29.942 | -28.05% | -27.94% | 7/7 |
| Cached | 1 | 2 | 41.369 | 29.754 | -28.08% | -27.74% | 7/7 |
| Cached | 1 | 3 | 26.722 | 28.761 | +7.63% | +7.02% | 0/7 |
| Cached | 1 | 4 | 41.182 | 45.747 | +11.08% | +11.11% | 0/7 |
| Native | 0 | 0 | 11.701 | 12.085 | +3.28% | +2.33% | 0/7 |
| Native | 0 | 1 | 29.374 | 14.639 | -50.16% | -50.33% | 7/7 |
| Native | 0 | 2 | 29.276 | 14.584 | -50.18% | -50.37% | 7/7 |
| Native | 0 | 3 | 11.921 | 13.999 | +17.43% | +16.02% | 0/7 |
| Native | 0 | 4 | 29.101 | 33.758 | +16.00% | +16.20% | 0/7 |
| Native | 1 | 0 | 11.910 | 12.053 | +1.20% | +1.20% | 3/7 |
| Native | 1 | 1 | 29.784 | 14.641 | -50.84% | -50.84% | 7/7 |
| Native | 1 | 2 | 30.379 | 15.344 | -49.49% | -49.14% | 7/7 |
| Native | 1 | 3 | 12.374 | 14.154 | +14.39% | +14.50% | 1/7 |
| Native | 1 | 4 | 29.915 | 33.912 | +13.36% | +14.20% | 0/7 |

## HLE output workload

A real guest MCR and two-instruction cached block establish the I-bit and code watches before timing. **No guest instruction executes in the timed loop**. The measured gateway writes a word, a byte, a 32-byte C string, or a 256-byte C string. String sizes include the terminator. Scalar cases execute 2,000,000 outputs; string cases 500,000. Data either occupies a separate page or starts 128 bytes after the translated code. All data outputs preserve the epoch in the coverage run.

The earlier scalar gateways omitted required code-change notification. Restoring that correctness costs about 2.2–3.7 ns per tested output, corresponding to 71–136% for gateways that previously took only 2.5–3.1 ns. These relative regressions are real component costs, not a whole-emulator slowdown estimate. The string gateway replaces repeated one-byte mapping/writes with the existing bounded bulk copy and one notification; tested 32-byte strings take about 82–84% less time and 256-byte strings about 97–98% less time. All 112 string pairs improve. Existing 512-byte cstring bounds and partial scalar semantics remain covered by separate functional regressions.

### HLE: gcc

| Writer | Placement | I | Before ns | After ns | Added ns | Change | Paired median | Improving pairs |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| word | different page | 0 | 3.043 | 5.357 | +2.314 | +76.06% | +72.53% | 0/7 |
| word | different page | 1 | 3.085 | 5.267 | +2.182 | +70.74% | +73.38% | 0/7 |
| word | same code page | 0 | 3.036 | 6.217 | +3.182 | +104.81% | +100.67% | 0/7 |
| word | same code page | 1 | 3.084 | 6.195 | +3.111 | +100.88% | +103.47% | 0/7 |
| byte | different page | 0 | 2.492 | 5.037 | +2.545 | +102.15% | +99.89% | 0/7 |
| byte | different page | 1 | 2.602 | 5.068 | +2.466 | +94.77% | +88.31% | 0/7 |
| byte | same code page | 0 | 2.595 | 5.895 | +3.300 | +127.17% | +129.45% | 0/7 |
| byte | same code page | 1 | 2.508 | 5.909 | +3.402 | +135.65% | +137.08% | 0/7 |
| cstring 32 | different page | 0 | 58.138 | 10.010 | -48.128 | -82.78% | -82.96% | 7/7 |
| cstring 32 | different page | 1 | 59.914 | 10.436 | -49.477 | -82.58% | -82.92% | 7/7 |
| cstring 32 | same code page | 0 | 59.502 | 10.762 | -48.740 | -81.91% | -81.84% | 7/7 |
| cstring 32 | same code page | 1 | 62.620 | 11.064 | -51.556 | -82.33% | -81.99% | 7/7 |
| cstring 256 | different page | 0 | 443.841 | 11.429 | -432.412 | -97.43% | -97.43% | 7/7 |
| cstring 256 | different page | 1 | 442.271 | 11.035 | -431.236 | -97.50% | -97.48% | 7/7 |
| cstring 256 | same code page | 0 | 435.179 | 12.110 | -423.069 | -97.22% | -97.20% | 7/7 |
| cstring 256 | same code page | 1 | 434.813 | 12.159 | -422.654 | -97.20% | -97.20% | 7/7 |

### HLE: clang

| Writer | Placement | I | Before ns | After ns | Added ns | Change | Paired median | Improving pairs |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| word | different page | 0 | 3.040 | 5.535 | +2.496 | +82.10% | +81.97% | 0/7 |
| word | different page | 1 | 3.056 | 5.527 | +2.471 | +80.86% | +80.68% | 0/7 |
| word | same code page | 0 | 3.049 | 6.727 | +3.678 | +120.65% | +118.30% | 0/7 |
| word | same code page | 1 | 3.031 | 6.769 | +3.738 | +123.34% | +123.43% | 0/7 |
| byte | different page | 0 | 3.036 | 5.394 | +2.357 | +77.64% | +79.05% | 0/7 |
| byte | different page | 1 | 2.820 | 5.292 | +2.472 | +87.67% | +87.67% | 0/7 |
| byte | same code page | 0 | 2.851 | 6.287 | +3.437 | +120.56% | +123.68% | 0/7 |
| byte | same code page | 1 | 2.799 | 6.120 | +3.321 | +118.62% | +118.85% | 0/7 |
| cstring 32 | different page | 0 | 68.642 | 10.742 | -57.900 | -84.35% | -84.54% | 7/7 |
| cstring 32 | different page | 1 | 67.873 | 11.080 | -56.793 | -83.68% | -83.87% | 7/7 |
| cstring 32 | same code page | 0 | 67.999 | 11.099 | -56.900 | -83.68% | -83.49% | 7/7 |
| cstring 32 | same code page | 1 | 68.228 | 11.049 | -57.179 | -83.81% | -83.95% | 7/7 |
| cstring 256 | different page | 0 | 509.716 | 11.900 | -497.816 | -97.67% | -97.67% | 7/7 |
| cstring 256 | different page | 1 | 510.043 | 12.049 | -497.994 | -97.64% | -97.64% | 7/7 |
| cstring 256 | same code page | 0 | 509.866 | 12.578 | -497.288 | -97.53% | -97.53% | 7/7 |
| cstring 256 | same code page | 1 | 504.148 | 12.200 | -491.949 | -97.58% | -97.58% | 7/7 |

## Interpretation and remaining limits

The external-watch optimization exchanges additional notification filtering for fewer false-positive translation invalidations. Its value depends on the proportion of writes to data sharing a translated code page. Correct notification remains necessary even if scalar-only workloads are slower. No game instruction histogram or device profile was available to weight these cases into an application-wide percentage. The no-notification controls move between -3.34% and +3.28%, demonstrating snapshot/layout and host effects even outside the edited notification path.

This hot MMU-off study does not measure raw ARM dispatch, cold collection/emission, arena allocation, MMU aliases, AArch64 host costs, iOS JIT permission handling, or frontend audio/video scheduling. Functional regressions cover several of those semantics separately. The JIT predicate candidates have their own controlled studies and remain rejected; their target gains must not be added to these adopted results.

## Reproduction

Use the final checkout's drivers for both builds. Create a before worktree at the public prior-review commit, then build matching source snapshots using `scripts/build_core_write_benchmarks.py` (see `--help` and the recorded manifests). Run the two `--verify` drivers first with profile1 binaries. With other local work stopped, run:

```sh
python3 docs/benchmarks/1.1.0-round5/writes/run-measurements.py \
  /path/to/final-checkout /path/to/before-build /path/to/after-build \
  /path/to/new-results
```

The recorded source, executable and driver hashes make this evidence reproducible without treating local audit commit IDs as public Git objects. `render-summary.py` validates all 1,008 retained sample/group/order records and regenerates these tables; it does not rerun or adjust a measurement.
