# Final round-five WebAssembly gate

The fresh freestanding C23 build and runtime gate pass at integrated commit
`865f88e2d095f71ac80935bd775a38f3b8985246`, tree
`e300f1f9b419f24da5f96b1e831640e47a9a32ce`. Both the production module and the
test module also pass the supplemental public save/load check with the new
v16 PWM trailer. No production source, existing test, or web asset was edited.

This is a functional gate in Node's WebAssembly engine. It does not measure
gameplay, browser presentation, real-time audio, native JIT speed, or physical
ARM hardware. The supplemental state fixture imports valid stopped-timer
values; it does not advance a guest program or independently establish timer
tick behavior.

## Build and provenance

| Item | Exact setting or result |
|---|---|
| Frozen source directory | `/tmp/gp32-round4` |
| Compiler | Ubuntu Clang 18.1.3, `--target=wasm32-unknown-unknown` |
| Linker | Ubuntu LLD 18.1.3 |
| Runtime | Node v24.19.0 |
| Build system | GNU Make 4.3 |
| C standard | `GP32EMU_C_STD=c23`, `GP32EMU_REQUIRE_C23=1`; every compilation uses `-std=c23` |
| Optimization | Existing Makefile `-O3 -g0`, function/data sections, linker section GC; no IPO option or profiling instrumentation added |
| Execution backend | Portable bytecode interpreter; native x64/A64 emission is unavailable for wasm32 |
| Existing block limits | `ARM_JIT_BLOCK_COUNT=32768`, `ARM_JIT_MAX_INSNS=96` |
| Linear memory | Existing initial 128 MiB, maximum 256 MiB |
| Object directory | `/tmp/gp32-round5-wasm/build` |
| Production output directory | `/tmp/gp32-round5-wasm/web` |

[manifest.json](manifest.json) records the source commit/tree, clean initial
status, SHA-256 hashes of the tracked source/header/runtime/build-policy files,
the complete command arrays, output hashes, and before/after source comparisons.
All recorded source hashes remain unchanged after the build and supplemental
checks. [versions.log](versions.log) retains the complete version output.
[build-runtime.log](build-runtime.log) contains every compiler and linker
command plus the unchanged runtime runner's raw output.

The build retains two non-fatal unused-function warnings in the portable CPU
translation unit: `fastmem_excludes` and `itlb_store`. The former is a native
emitter certificate helper and has no caller in this backend. No warnings were
suppressed to obtain this result.

| Output | Bytes | SHA-256 |
|---|---:|---|
| Production `gp32_wasm_core.wasm` | 314,246 | `4ab48a805cb46dad44f58afcd44191c9eae4d2f6d29b276578352071b8e1883d` |
| `wasm_runtime_test.wasm` | 317,389 | `4d85a92ec70b803fbea63526e6a1774ca14d5b1137ef51c7fc25303db09fb3ed` |

The two modules were linked from the same core objects. The test module adds
the existing exported runtime fixture. Production imports `pcfx.read_host_file`
and `pcfx.sin`; the test module additionally keeps `pcfx.now_ms` reachable for
the clock check. Both instantiate with the matching host import object.

## Existing runtime gate

The checked-in [C fixture](../../../../tests/wasm_runtime_test.c) and
[Node runner](../../../../tests/wasm_runtime_test.mjs) ran unchanged. They pass:

- Constant positive and negative PCM at four source/destination rate pairs,
  with all 256 filter phases checked for unity coefficient gain.
- Monotonic host-clock conversion, including a value beyond 32-bit milliseconds.
- Growing VFS writes and content checks, full-directory close failure,
  borrowed/owned/pending-handle cleanup, and allocation failure preserving
  existing file contents.
- Thirty-two successive browser-side save operations, with the retained staging
  allocation and temporary VFS publication/removal path exercised on every save.

The saved image is **9,290,654 bytes** and the original save-only check holds
heap usage at **38,340,768 bytes**. The image is 92 bytes larger than the prior
round's 9,290,562-byte image, matching the appended PWM trailer. The existing
runner checks repeated saves; it does not call the load API.

## Supplemental v16 save/load check

[state-roundtrip.mjs](state-roundtrip.mjs) is an evidence-only public-API driver;
it was not added to or substituted for the existing test target. Its complete
output is [state-roundtrip.log](state-roundtrip.log).

It creates a machine with a synthetic 32-byte BIOS and saves its initial state.
The BIOS never executes. The driver verifies `GP32STATEv0016` and the trailing
92-byte `PWM1` section, then changes all five stopped timers' 16-bit counts and
fractional phases on every import. Reload values and pending bits remain valid.
Every load gets a distinct `gp32_wasm_malloc` import allocation because the
public wrapper takes ownership of and frees that allocation.

Each module passes **32 load/save round trips** and preserves all 92 trailer
bytes on every round trip, covering **160 distinct timer count/phase pairs per
module**. State size, CPU PC/cycles, and empty VFS staging remain correct. The
production and test modules produce the same final state hash:
`9c0dbbb38a6f0257239f8998cd4bd7a662c1fdef3e7914b874288e6d47e4b8ae`.

Each module also rejects a v16 image truncated by one trailer byte, reports
the existing wrapper's `ERROR`/`BAD_STATE` status, preserves CPU PC/cycles, and
removes the failed import's VFS staging. This negative check does not claim that
the public wrapper can continue saving while in its error state.

### Heap placement is bounded, with an alternating RAM allocation

The first supplemental harness incorrectly assumed that save/load would have
the same constant heap top as save-only operation. The second round trip
increased the reported heap top by **8,388,624 bytes**, exactly an 8 MiB RAM
allocation plus its 16-byte header. That initial failure is retained below.

The side-effect-free [allocation-map diagnostic](state-allocation-map.mjs)
walks the allocator's complete header chain in the already-built module. It
checks header validity and the exact reported heap endpoint; it does not alter
the allocation sequence. [Its raw output](state-allocation-map.log) records all
32 samples for each module.

| Quantity after each successful round trip | Observed result in both modules |
|---|---|
| Heap top relative to heap base | Alternates exactly 38,340,768 / 46,729,392 bytes |
| Live allocation count | 10 throughout |
| Live payload sizes recorded in headers | 19,604,152 or 19,604,167 bytes |
| Large live allocations | One 8 MiB RAM image and one 9,290,654-byte save staging buffer |
| Save staging address | Fixed for the complete run |
| Replacement RAM address | Alternates around the retained staging allocation |

The state loader allocates and validates a replacement RAM image before freeing
the old one. The first-fit WASM allocator can place the replacement beyond the
retained save buffer on one iteration and below it on the next. Tail trimming
then returns the lower heap top on alternating iterations. The 15-byte live
header-size difference is allocator slack when a free block cannot be split;
the live block count does not grow. This trace shows no accumulating leak in
the tested sequence. It is not a general bound for arbitrary games, RAM sizes,
or state content.

The final public-API check requires the initial two-placement pattern to repeat
through all remaining rounds and caps it at one additional RAM allocation over
the first round trip. The unchanged save-only test still requires strict heap
equality on every save.

## Retained failed harness assumptions

These failures were reported before any production edit; none led to one.

1. [first-state-roundtrip.mjs](first-state-roundtrip.mjs) sliced the 14-byte
   state magic at 13 bytes. [The raw failure](first-state-roundtrip.log) occurred
   before the first state load. [first-manifest.json](first-manifest.json)
   records that exact first attempt.
2. [second-state-roundtrip.mjs](second-state-roundtrip.mjs) corrected only the
   header length and then failed its unsupported constant-heap assertion on
   round trip two. [second-state-roundtrip.log](second-state-roundtrip.log) and
   [second-manifest.json](second-manifest.json) preserve that version. The
   allocation-map diagnostic above establishes why the assertion was too strong.
3. The initial [allocation diagnostic](first-state-allocation-diagnostic.mjs)
   completed the positive round trips after recording heap sizes but then
   expected a save to remain callable after a deliberately rejected import.
   [Its failure](first-state-allocation-diagnostic.log) is also preserved.
   `set_error` has always moved this wrapper into `ERROR` status; its save API
   requires `RUNNING`. The final negative check follows that existing contract.

The independent positive PWM tail values and expected load/save results did not
change during these corrections. The final gate passes without a production
change or an edit to the existing regression tests.

## Reproduction

Use a checkout at the exact frozen commit. The object/output directory must be
fresh; [run-gate.py](run-gate.py) refuses to overwrite an existing one. Its
recorded invocation was:

```sh
python3 docs/benchmarks/1.1.0-round5/wasm/run-gate.py \
  /tmp/gp32-round4 /tmp/gp32-round5-wasm \
  /tmp/gp32-round5-jit/docs/benchmarks/1.1.0-round5/wasm
```

The actual Make command, run with `/tmp/gp32-round4` as the working directory:

```sh
make -f Makefile.wasm build test GP32EMU_C_STD=c23 GP32EMU_REQUIRE_C23=1 \
  WASM_CC=clang-18 WASM_LD=wasm-ld-18 NODE=node \
  BUILD_DIR=/tmp/gp32-round5-wasm/build OUT_DIR=/tmp/gp32-round5-wasm/web -j2
```

The final supplemental command:

```sh
node /tmp/gp32-round5-jit/docs/benchmarks/1.1.0-round5/wasm/state-roundtrip.mjs \
  /tmp/gp32-round5-wasm/web/gp32_wasm_core.wasm \
  /tmp/gp32-round5-wasm/build/wasm_runtime_test.wasm
```

The allocation diagnostic uses the same two module arguments with
`state-allocation-map.mjs`. No module was rebuilt for any supplemental or
diagnostic execution. The source manifest and output hashes identify the same
frozen artifacts throughout.
