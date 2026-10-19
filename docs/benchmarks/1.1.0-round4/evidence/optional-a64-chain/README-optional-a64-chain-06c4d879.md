# Optional AArch64 two-way native chaining candidate

**Status: unadopted.** This patch is an isolated experiment against the final
Round 4 source. It is not part of the default core. Functional checks pass under
QEMU; physical AArch64 hit/miss performance remains unmeasured.

## Exact source and patch

- Frozen base: `06c4d879543f90d1003d174718d738e27a8ed6b8`.
- Frozen base tree: `1fd43d466c20f9dd3e470a6a1f065f3ca283f70e`.
- Tested optional commit: `e80f8348cdd913159a5d26ae8dea9f04f0ded12e`.
- Tested optional tree: `b3d73eff1c761e69f6239e78a733bf6b4fed914b`.
- Optional branch: `audit/round4-a64-chain`.
- Patch: [optional-a64-chain-06c4d879.patch](optional-a64-chain-06c4d879.patch).
- Patch SHA-256: `e0070d1271f4979b7dfa8547c0cb53197e219531ca3943055fbdb6860a3a5a11`.

The candidate was first preserved as `0384050cd9f50bfc8b35965fcb13479853f2f89b`
on base `618e6600aa201ac182498c22a301b8ec6c03f1ae`, then rebased cleanly.
`git range-diff` reports the same patch. Applying the exported patch to an
isolated index of the frozen base reconstructs the tested optional tree exactly;
see [application proof](optional-a64-chain-06c4d879-apply-check.log).

The [manifest](optional-a64-chain-06c4d879-manifest.json) records source, patch,
binary and log hashes, exact compiler options and test commands. `SHA256SUMS`
checks every packaged file except itself.

## Behavior and limits

A64 `rt_hop` currently probes the first slot of the target block's two-way set.
The candidate probes the second 32-byte slot only after a first-slot tag,
generation or epoch mismatch. The second slot must match the expected PC/valid
header and live generation/epoch, then rejoins the original native, poll,
deferred-target and budget acceptance checks and hop tail.

The runtime adds **seven cold instructions (28 bytes)**. A successful first-way
hit executes no additional instructions; branch displacements do change. A miss
can perform additional loads and comparisons. Existing runtime-capacity failure
handling remains active. No clock, guest instruction semantics or public API is
changed by design. This is not a measured speedup or an architectural bug fix.

## Final-base functional evidence

Both direct builds and both QEMU runs exited zero, with empty build logs.

- [Focused test](optional-a64-chain-06c4d879.log): **30** first/second-way,
  ragged-budget and rejection scenarios pass. It checks exact architectural
  state and actual chain coverage, then directly enters native code to ensure
  dispatcher repair cannot hide an invalid hop. Guards include invalid or stale
  entries, uncompiled/poll/deferred targets, short budget, IRQ/FIQ and wrong tag.
- [Full JIT differential](optional-a64-chain-full-jit-06c4d879.log): **115,386 JIT
  events, zero fallbacks**, including the integrated forward-branch cases.

At the original base, the new focused fixture had eight expected structural
coverage failures across its 30 scenarios: ready second-way successors returned
to the dispatcher. Those were chaining-coverage assertions, not failed guest
architectural-state comparisons. The original optional full JIT run passed
114,832 events; the larger final count reflects integrated test additions.

## Reproduce in an isolated checkout

Use a checkout of the frozen base above. Apply only the attached optional patch:

```sh
git apply --check /absolute/path/optional-a64-chain-06c4d879.patch
git apply /absolute/path/optional-a64-chain-06c4d879.patch
```

The tests include the core themselves; do not link `src/arm920t.c` separately.
Run these commands from that patched checkout (no more than two compile jobs):

```sh
aarch64-linux-gnu-gcc -std=c2x -O3 -DNDEBUG -DGP32EMU_CPU_PROFILE=1 -DARM_JIT_A64_LINK=1 -Iinclude -Isrc tests/arm_chain_way_test.c -o /tmp/gp32-round4-a64-optional-final-chain
aarch64-linux-gnu-gcc -std=c2x -O3 -DNDEBUG -DGP32EMU_CPU_PROFILE=1 -DARM_JIT_A64_LINK=1 -Iinclude -Isrc tests/arm_jit_test.c -o /tmp/gp32-round4-a64-optional-final-jit
qemu-aarch64 -L /usr/aarch64-linux-gnu /tmp/gp32-round4-a64-optional-final-chain
qemu-aarch64 -L /usr/aarch64-linux-gnu /tmp/gp32-round4-a64-optional-final-jit
```

Recorded compiler: `aarch64-linux-gnu-gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`. Recorded runner: `qemu-aarch64 version 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18)`. CPU profiling is
explicitly enabled for coverage assertions; these are functional runs, with no
performance timing. Physical-device adoption would require paired profile-off
first-way-hit, second-way-hit and miss-heavy measurements with exact state checks.
