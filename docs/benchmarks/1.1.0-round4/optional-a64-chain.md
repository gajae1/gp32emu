# Optional AArch64 second-way block chaining

**Not adopted in the default core.** The exported
[patch](candidates/a64-chain-second-way.patch) adds a second-way probe to the
A64 runtime's direct block hop and a standalone test. No CMake default target
or product source on this review branch includes it.

The source code already has a two-way lookup table, while the native runtime
hop probes only its first way. A valid second-way target therefore returns to
the dispatcher and executes correctly. This candidate reduces that structural
fallback in an intentionally colliding synthetic workload; it does not fix a
demonstrated guest-state error.

## Mechanism and limits

On first tag/generation/epoch mismatch, seven cold instructions (28 bytes)
load and compare the second way. A match rejoins the existing acceptance
checks for native readiness, poll/deferred status, remaining budget, and
interrupt boundaries. It uses the existing generation/epoch and hop tail.
A successful first-way path executes no extra instructions, although branch
displacements change. The bounded runtime allocation remains unchanged.

Miss-heavy paths incur extra probing. Physical AArch64 hit/miss distributions,
branch prediction, cache pressure and elapsed-time effects have not been
measured. QEMU verifies generated instructions and state; its time is not
evidence of speed on Android, iPhone or a handheld.

## Functional and structural checks

The focused test has 30 scenarios: both ways at ten ragged budgets, plus ten
direct-entry rejection controls. A real hash collision keeps a decoy in the
first way. Direct entry prevents dispatcher lookup/repair from hiding an
unsafe hop. The tests cover stale generation/epoch, invalid or unready native
entries, poll/deferred entries, insufficient budget, masked IRQ/FIQ and a
wrong tag.

The original control at integration step `618e660` had eight structural
assertion failures and zero architectural mismatches. With a second-way
target and budget 1,000 it used 251 native calls and 249 hops. The candidate
uses one call and 499 hops; both retire 1,000 instructions and agree on state.
These counts are not a performance multiplier.

The candidate was then cleanly rebased onto production source commit
`06c4d879543f90d1003d174718d738e27a8ed6b8`. On those final product bytes:

| Gate | Result |
|---|---|
| [Focused final run](evidence/optional-a64-chain/optional-a64-chain-06c4d879.log) | 30 scenarios pass |
| [Full final JIT differential](evidence/optional-a64-chain/optional-a64-chain-full-jit-06c4d879.log) | 115,386 JIT events; zero fallbacks |
| [Patch application proof](evidence/optional-a64-chain/optional-a64-chain-06c4d879-apply-check.log) | Exact tested candidate tree reproduced |

Both use GCC 13.3.0, C23-required dialect `-std=c2x`, `-O3 -DNDEBUG`,
`GP32EMU_CPU_PROFILE=1`, `ARM_JIT_A64_LINK=1`, and QEMU 8.2.2. The two
compilations and executions exited zero. The default review later adjusts two
unrelated legacy test fixtures and documentation; the tested production and
optional-test bytes remain unchanged. Final patch applicability is checked
again against that validated fixture freeze in the
[final-source application proof](evidence/optional-a64-chain-final-source-apply.json).
The resulting production and standalone JIT-test bytes equal the executed
candidate; documentation-only additions do not affect the patch's paths.

## Applying and checking the candidate

Use a separate checkout of the review branch. The patch changes only
`src/arm920t_jit_a64.inc` and adds `tests/arm_chain_way_test.c`.

```sh
git apply --check docs/benchmarks/1.1.0-round4/candidates/a64-chain-second-way.patch
git apply docs/benchmarks/1.1.0-round4/candidates/a64-chain-second-way.patch
aarch64-linux-gnu-gcc -std=c2x -O3 -DNDEBUG \
  -DGP32EMU_CPU_PROFILE=1 -DARM_JIT_A64_LINK=1 -Iinclude -Isrc \
  tests/arm_chain_way_test.c -o /tmp/arm-chain-way
qemu-aarch64 -L /usr/aarch64-linux-gnu /tmp/arm-chain-way
aarch64-linux-gnu-gcc -std=c2x -O3 -DNDEBUG \
  -DGP32EMU_CPU_PROFILE=1 -DARM_JIT_A64_LINK=1 -Iinclude -Isrc \
  tests/arm_jit_test.c -o /tmp/arm-chain-jit
qemu-aarch64 -L /usr/aarch64-linux-gnu /tmp/arm-chain-jit
```

The [package manifest](evidence/optional-a64-chain/optional-a64-chain-06c4d879-manifest.json)
records source, binary and log hashes and exact commands. The patch SHA-256 is
`e0070d1271f4979b7dfa8547c0cb53197e219531ca3943055fbdb6860a3a5a11`.
The tested base/candidate A64 source hashes are respectively
`482aa799a352baa57e7d437159738a2c33da6a78e187739e8de82a8b044d8140` and
`519e323ca86bac8a6880b293b9ac9d2b00b472fc38d0f7d0d1852a5b4137b67f`.

A future adoption decision should measure the normal first-way success,
second-way success and repeated misses separately on the intended physical
AArch64 device, alongside real workload frequency. Existing functional
guards remain requirements for that experiment.
