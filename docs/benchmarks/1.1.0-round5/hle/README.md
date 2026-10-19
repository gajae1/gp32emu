# HLE output coherence and external word-watch regression

The final `gp32_hle_memory` fixture contains 155 cases, including fourteen
writers exercised with raw/cached/native execution and CP15.I=0/1. Before,
60 assertions fail: 56 stale instruction values and four stale palette PCs;
raw controls pass. After, all cases pass. Literal synthetic instructions,
register/PC checks and real guest SWI gateways are used where applicable.
The test preserves the existing bounded zero-fill checks.

The external fixture runs 12,920 span, instruction-replacement and MMU-fallback
cases using an independent byte-interval oracle: 12,840 interval checks,
72 instruction-replacement cases and eight MMU fallback/replacement cases.
They include page/bitmap/RAM edges, partial words, large/clipped lengths and
MMU aliases. Before, 4,356 assertions identify unnecessary page-granular epoch
changes for non-code data. After, those assertions pass and real instruction
overlaps still invalidate. These are performance-invariant failures, not
4,356 wrong guest instruction results.

`before-identity.json` pins the public/local baseline and the unchanged final
fixture hashes. All before/after test and build output is retained. The first
external build attempted the new target before CMake had regenerated; its
setup failure is in `before-external-build-setup-failure.log`. The corrected
build and actual failing baseline test follow separately; no result is
inferred from the setup failure.

Run the final fixtures through CTest's `gp32_hle_memory` and
`arm_external_write` entries. The complete fresh integrated matrices and input
hashes are in [verification](../verification/README.md). For historical before
execution, the reproduction history contains the baseline and experiment
commits; the public prior-review source has the same baseline tree. The same
new fixtures must be used on both sources.

The production change notifies scalar, string, palette, stub/callback and
instruction-patching writes after they occur. Bounded cstrings and bulk
installations use one range notification. Existing RAM word watches refine a
positive page hit; MMU/untracked fallback and I=1 fetch tracking remain.
See [the Korean rationale](../../../REVIEW-1.1.0-ROUND5.ko.md) and
[the component measurements](../writes/README.md) for actual costs and gains.
