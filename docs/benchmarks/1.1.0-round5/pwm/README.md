# Round 5: PWM live counters, reload ordering and saved phases

Review date: 2026-10-18. This is functional evidence, not a performance benchmark.
No ROM or physical device was used. No release or remote Git reference was
changed from this isolated worktree.

The starting commit was local
`85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f`, whose tree
`396aaf687ce5d1ead21db9af44f25185b44ec698` exactly matched public
`2bace9afd9c3d1e5e7cac9d23379e2f60d167fec`. Work was isolated in
`/tmp/gp32-round5-pwm`, with builds in `/tmp/gp32-round5-build-pwm`.
The reviewed product/test commit is
`dc871e6f8f736669aa4966bfac81610499b37cc2` (tree
`f479e4e5943f5b41ecd417df4d69f8a7a4ae2a3a`).
The [round-four review](../../../REVIEW-1.1.0-ROUND4.ko.md) and its unchanged
[25-observation probe](../../1.1.0-round4/evidence/pwm-model-gap.c) are the
starting defect record.

## Findings and implementation

The former implementation derived TCNTO from the current TCNTB register and an
elapsed-period accumulator. Consequently, changing a future buffer changed the
present count, stopping exposed the buffer, stopped ticks discarded progress,
and a manual update did not actually load a counter. All five timers showed the
same defect. Runtime state now separates each live 16-bit count, captured reload,
pending reload bit and fractional timer edge from the software-visible buffer.

The primary source is Samsung's
[S3C2400 manual](https://datasheets.chipdb.org/Samsung/S3C2400.pdf), chapter 10.
The downloaded PDF SHA-256 is
`04c46ac5c1aa9ef5805ca27f69357422ea3c610ce1b9af8cd5d8647ca1a38557`.
The waveform in Figure 10-2 was visually inspected, alongside the text:

| Manual location | Basis for the modeled behavior |
| --- | --- |
| 10-4, double buffering | TCNTB changes a future duration without changing the current counter. |
| 10-5, initialization and stop | Initialization needs a manual update; a forced stop retains TCNT. |
| Figure 10-2 on 10-3; step 4 on 10-6 | Reaching zero requests service and captures the reload; the following timer clock loads it. |
| 10-10, DMA request mode | The selected timer issues a DMA request instead of its timer interrupt. |
| 10-11–10-12, TCFG0/TCFG1 | Timers 0/1 share one prescaler; timers 2/3/4 share the other, with the documented internal dividers. |

For a positive manually loaded value N, the chosen initial phase gives the first
terminal request after N decrement edges. Zero is observable for one timer edge;
steady positive auto-reload intervals therefore take N+1 edges. A TCNTB write
from the terminal request's DMA cannot replace the value already captured for
the next reload. TCMPB still does not change the count or terminal period; TOUT
waveform generation is outside this change.

The phase numerator accumulates `CPU cycles * PCLK`; one timer edge consumes
`RUN * ((prescaler + 1) * input_divider)`. This avoids combining a floored whole
period with a separately rounded-up decrement duration. Writes to TCNTB do not
invalidate the derived clock. The prescaler, mux and clock setters preserve the
fraction through an explicit conversion.

The scheduler advances all five timers to the earliest observable request before
performing its DMA bus action. It preserves chronological samples of another
timer and starts a DMA-enabled timer only for the time remaining after that bus
write. All same-instant reload captures precede request dispatch. Already pending
interrupt repetitions can be skipped arithmetically, so a `UINT32_MAX` raw tick
with a count-one repeating timer does not loop over a billion redundant requests.
Accepted DMA requests remain individually ordered because they have observable
bus effects.

An additional regression found during this implementation prevented an unsafe
form of that coalescing. Timer 1 requests at cycles 4, 10 and 16; timer 0's DMA
acknowledges it at cycle 20; the next timer 1 request is at 22. The first scheduler
replayed an earlier, coalesced request after the cycle-20 acknowledgement in
coarse and uneven partitions. Bulk advancement now reports a terminal request
only when it occurs exactly at that interval's endpoint. The scheduler already
splits at the first observable IRQ or accepted DMA request. The unchanged test
fails twice before this correction and passes afterwards; see
[dma-ack-before.log](dma-ack-before.log) and
[dma-ack-after.log](dma-ack-after.log).

### Explicit emulator policies and limits

These decisions make the model deterministic without claiming undocumented
silicon behavior:

- The first timer tick starts with a full interval. Manual updates and a forced
  stop preserve the fractional tick. Exact asynchronous prescaler alignment,
  and a manual-update bit held asserted across clocks, are not silicon-verified.
  Tests use the documented pulse-and-clear programming sequence.
- A manual/reset value of zero is quiescent until another manual load. It is
  not silently treated as 65536. An auto-reload that captured zero loads it on
  the next edge and then becomes quiescent. This is a stated zero-initialization
  policy, not a claim about an unspecified edge case on a physical chip.
- One-shot completion retains zero and clears its start bit, keeping the
  existing emulator's completion convention. A bare restart of an exhausted
  zero does not load TCNTB.
- Simultaneous requests dispatch by timer index, then DMA channel index. This
  is deterministic bus-model ordering, not a model of silicon arbitration.
- External TCLK pins are not exposed by this board API. The existing `/16`
  fallback for external/reserved mux selections is retained and is not an
  emulation of an external waveform.
- This work orders events within PWM. The preexisting LCD/IIS/PWM order of a
  raw `s3c2400_tick()` is unchanged; cross-device DMA arbitration is not made
  cycle-exact by this change.

## Saved-state compatibility

Whole-machine writers now emit the 16-byte `GP32STATEv0016` header, including
the two trailing zero bytes. Standalone SoC writers use the eight-byte
`GP32SO10` header. New loaders retain the existing whole-machine v2–v12/v14/v15
branches and standalone GP32SOC6–9/raw-body paths. The prior v13 exclusion is
unchanged. Older whole-machine builds reject the new magic rather than
misinterpreting a new state as their own layout.

The old native-layout SoC image, RAM, SmartMedia, audio and earlier phase/codec
sections keep their byte layout. A fixed 92-byte little-endian trailer follows
the codec. No native struct padding or spare register bits encode the new state:

| Trailer offset | Bytes | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `PWM1` |
| 4 | 4 | Original PWM phase clock, unsigned little-endian |
| 8 | 4 | Pending reload bitmap, bits 0–4 only |
| 12 + 16×timer | 4 | Live count, 0–65535 |
| 16 + 16×timer | 4 | Captured reload, 0–65535 |
| 20 + 16×timer | 8 | Fractional phase numerator, unsigned little-endian |

The loader requires a nonzero clock, a valid bitmap, 16-bit counts/latches,
zero live count when a reload is pending, zero unused latch when it is not,
and a phase strictly below `saved_clock * divider`. A stopped timer may retain
a pending reload. Every field is staged and checked before RAM, SmartMedia,
PCM, registers or IRQ state are replaced. The optional phase output is also
published only after success. Arithmetic splits at the 32-bit clock denominator,
so even the largest accepted source phase can convert without a 64-bit product
overflow.

The old body still contains a best-effort cycle-unit projection for its former
PWM accumulator fields. New loaders treat the explicit trailer as authoritative.
Old states have no way to reconstruct a lost live count, stop fraction or
already captured reload. Migration therefore preserves the old observable
counter where representable, keeps TCNTB itself intact, and treats an old
running auto-reload zero as a pending reload without inventing an interrupt at
load time. The old 65536 readback for a zero buffer becomes literal zero when
that unrepresentable value is encountered. A stopped legacy state resumes from
the former buffer readback with no fractional tick. This compatibility is
deliberately approximate where the original format omitted information.

The migration fixture is an actual output from the baseline GP32SOC9 writer,
not a new image with a renamed header. Its 848482-byte original has SHA-256
`e1f722a14171d1e4dccb7f2990701951976e5da369d57024297b0670b303efe2`.
[legacy-writer.c](legacy-writer.c) and [legacy-writer.log](legacy-writer.log)
record its construction. The checked-in sparse fixture in
`tests/pwm_legacy_soc9_fixture.h` reconstructs those exact bytes. Its old
observable counts are 185, 117, 65521, 151 and 4. This native-layout legacy
fixture requires its original little-endian LP64 ABI; the new trailer itself
does not depend on that ABI.

### Exact phase through host-speed normalization

Round four's runtime checkpoint already prevented a save or rejected load from
consuming live peripheral time. That checkpoint remains in use. Successful
whole-machine saves/loads still normalize the older sections through the nominal
CPU speed, preserving their established format and behavior.

The new PWM trailer additionally preserves the original numerator and denominator
from that checkpoint. A validated incoming PWM fraction is converted directly
to the final host clock after a successful load. It does not go through a second
floor at the nominal clock. This matters at 133%: the preliminary save/load
path had 15 actual count divergences during 400 single-cycle observations of an
uninterrupted source and its restored clone. The public observations and original
exploratory probe are retained in [same-speed-before.log](same-speed-before.log)
and [same-speed-probe.c](same-speed-probe.c).

That exploratory probe also compared the entire old wire image. Its broad
comparison includes non-PWM phase conversion, which remains unchanged in this
patch. Its unchanged [after-run](same-speed-after.log) retains those two broad
wire-image mismatches, while all 15 timer-count divergences are gone. It is not
a claim that every older peripheral section becomes exactly
reversible at arbitrary host speeds. The committed regression compares the
entire PWM trailer at save/load boundaries, then compares each timer count and
each pending timer IRQ after every single-cycle advance for 400 cycles, at
100%, 133% and 150%, for both file and buffer loads. Cross-speed checks
derive the expected PWM numerator with one independent mathematical conversion;
existing CPU/LCD/PCM assertions retain their established reference.

## Regression evidence

The event oracle in `tests/s3c2400_pwm_model_test.c` uses only public SoC APIs and
walks individual reference clock quanta. It does not include production code or
call a production timing helper to compute expected values. It covers all five
timers, byte/halfword/word buffer and manual accesses, stopped fractions, literal
zero, one-shots, captured reloads, integer and fractional clocks, and ordered DMA
samples/buffer writes/starts/acknowledgements. Deterministic generated actions
include stop/start, manual update, buffer changes and auto-reload changes.

| Evidence | Result |
| --- | --- |
| Unchanged round-four probe on baseline | 25 observations, 25 mismatches |
| Unchanged round-four probe on new model | 25 observations, zero mismatches |
| Final oracle against baseline SoC | 49 scenarios, 3786 checks, 2504 assertion failures |
| Final oracle against new model, including maximum tick | 50 scenarios, 3789 checks, zero assertion failures |

The pre-change maximum-tick stress case was intentionally not run: the old code
iterates every redundant period. It is separately enabled by the `large` test
argument and included in the new CTest registration. The shared oracle's
remaining checks are unchanged across the baseline/final comparison in
[final-before-after.log](final-before-after.log).

`tests/s3c2400_pwm_state_test.c` covers mixed stopped/running states, both forms
of zero phase, differing TCNTB and captured reloads, continued execution after
load, all 92 byte-truncation cuts, invalid field combinations, the real legacy
capture, standalone cross-speed PWM restoration, and maximum valid/one-past
phase bounds. Rejections compare complete saved SoC bytes, including different
RAM, card and queued PCM content. The whole-machine speed suite covers CPU
transactionality, v15 decoding with its CPU translation section retained,
malformed/missing v16 tails, repeat saves, file/buffer rejection and continuation.

The initial integration suite failed in five existing tests that had relied on
starting a counter merely by writing TCNTB. Fixtures now issue explicit manual
loads. Two deadline assertions also follow the independently documented
one-to-zero request edge and retain their clock-transition checks. No tests were
disabled. The historical result is retained in
[initial-integration-ctest.log](initial-integration-ctest.log).
A subsequently added version fixture initially copied beyond the 14-character
header literal and compared a derived IIS dirty-cache flag without normalizing
it as the existing round-four fixture does. Those test-only errors were fixed;
their failed run remains in
[gcc-ctest-version-fixture-initial.log](gcc-ctest-version-fixture-initial.log).

Final execution results for the product commit:

| Configuration | CTest result | Focused raw output |
| --- | --- | --- |
| GCC 13.3, Release | 56/56 passed | Model 50 scenarios / 3789 checks; state 105 scenarios; whole-machine speed 62 scenarios; zero failures |
| Clang 18.1.3, Release | 56/56 passed | Same scenario/check totals and zero failures |
| GCC 13.3, UBSan | 3/3 focused suites passed | Same scenario/check totals, zero failures and no UBSan diagnostic |

See [gcc-ctest.log](gcc-ctest.log), [clang-ctest.log](clang-ctest.log),
[ubsan-ctest.log](ubsan-ctest.log), and the unmodified selected CTest entries in
[focused-results.log](focused-results.log). The state scenario counter counts
the 100 truncation/malformed-tail cases, two ordinary round trips, one legacy
migration and two maximum-phase cases. The standalone cross-speed subcheck also
runs inside the transaction scenario function.

## Commands and toolchains

All final commands and full functional test results are recorded in the adjacent
logs. No reported duration is a performance measurement. Builds/tests were
paused during the parent review's controlled JIT timing windows.

```sh
/tmp/gp32-round4-tools/cmake/data/bin/cmake \
  -S /tmp/gp32-round5-pwm -B /tmp/gp32-round5-build-pwm/gcc \
  -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_BUILD_TYPE=Release \
  -DGP32EMU_REQUIRE_C23=ON -DGP32EMU_BUILD_TESTS=ON \
  -DGP32EMU_CPU_PROFILE=ON -DGP32EMU_ENABLE_IPO=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
/tmp/gp32-round4-tools/cmake/data/bin/cmake \
  --build /tmp/gp32-round5-build-pwm/gcc --parallel 2
/tmp/gp32-round4-tools/cmake/data/bin/ctest \
  --test-dir /tmp/gp32-round5-build-pwm/gcc --output-on-failure --parallel 2
```

Clang uses the same options with `clang-18` and the `clang` build directory.
The UBSan build uses GCC, `RelWithDebInfo`, and
`-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`, and runs
the PWM oracle, PWM state and whole-machine speed suites. Exact invocations are
in [ubsan-configure.log](ubsan-configure.log) and the final build/test logs.

[toolchain.log](toolchain.log) records actual compiler commands and macros.
CMake 4.4.4 requests C23 without falling back to C17. GCC 13.3 uses `-std=c2x`
and reports `__STDC_VERSION__=202000L`; this is its draft-dialect implementation,
not a claim of complete final-C23 support. Clang 18.1.3 uses `-std=c23` and
reports `202311L`. CPU profiling is enabled and IPO disabled in every build.
Preexisting warning output is retained, including GCC's two audio-boundary
type-limit warnings and Clang's unused test-fixture helper warnings.

`SHA256SUMS` identifies the final reviewed source/tests and retained probe
sources. Parent integration is responsible for the combined branch's final
whole-core and target-platform matrix; these isolated results establish the PWM
change on its own baseline.
