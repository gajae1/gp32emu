# GP32 on H700: performance research and next probes (2026-10-03)

## Target and measurement

The practical target is **60 emulated video refreshes per real second**, with
uninterrupted PCM and correct game timing, through SpruceOS/RetroArch on an H700
class device. This does not promise that a game originally animating at 30 fps
will acquire 60 unique animation frames. A headless benchmark above 60 fps is
necessary headroom, not sufficient proof of 60 fps in RetroArch.

The tested RG SP has a 720x480 screen and an H700 with four Cortex-A53 cores
advertised at 1.5 GHz [Anbernic RG SP specification](https://anbernic.com/products/rg-sp).
Its core currently executes GP32 CPU and peripheral events predominantly in
one ordered emulation stream, so four host cores do not imply four times the
single-game throughput. The device was observed with the `conservative` CPU
governor, 480 MHz idle frequency and 1512 MHz maximum; comparisons must record
actual frequency and temperature or use interleaved runs at a matched clock.

Earlier H700 core-only windows included Blue Angelo gameplay at 54.705 fps
(600 warmup / 300 measured frames) and Little Wizard character selection at
47.846 fps (2400 / 300, observed 1512 MHz).
Wizard first improved from 37.226 to 45.108 fps in a complete matched-clock
ABBA comparison (+21.18%); a later GPIO dispatch comparison added 0.82%.
AArch64 immediate and block-prologue improvements then measured +3.02% in
Wizard and +1.55% in Blue's 600/300 window, each in a full matched-clock ABBA.
Resolving RAM once for the existing Blue shadow repair then improved Blue by
14.09%, with Wizard flat. A separate block-header validation change measured
another 1.13% in Blue and 1.89% in Wizard against that framebuffer candidate.
Blue's original 60/180 window still has a ramping first baseline and its
aggregate ratio is disqualified. Do not combine different windows or multiply
independent experimental gains. Those historical samples were below 60 before
RetroArch overhead. Later workload-specific results are recorded below; the
resume18 Wizard 2400/300 window reaches 60.319 core fps, with little headroom
for frontend overhead and no whole-game guarantee.
These are **different fixed scenes**, not game-wide minima or a ranking of
the hardest GP32 titles. See `PORTABILITY_PROGRESS.md` and the raw JSON in
`F:/GP32/results/` for workloads, hashes and clock caveats.

The same extracted Blue Angelo gameplay state also ran at 532.537 core-only
fps in one Win64 `-O3`/x86-64 JIT run on an AMD Ryzen 7 9800X3D. CPU state,
video and PCM hashes matched H700 exactly. The saved result is
`F:/GP32/results/blue-gameplay-pc-win64.json`. This demonstrates ample
headroom on this PC for this scene; it does not establish whole-library or
RetroArch runtime compatibility on PC.

## What GP32 overclocking means

Samsung's [S3C2400 manual](https://datasheets.chipdb.org/Samsung/S3C2400.pdf)
describes an ARM920T with a programmable MPLL and states a 150 MHz maximum at
1.8 V for that revision. This is a chip specification, not proof of what every
assembled GP32 could sustain. The contemporary [GP32X FAQ](https://pyra-handheld.com/boards/threads/gp32x-collaborative-faq.1667/)
reports units that stopped below 166 MHz and units that ran at 166 MHz, with
RAM quality and battery life identified as constraints. A developer's
[clock-domain explanation](https://pyra-handheld.com/boards/threads/overclocking.3110/)
notes that peripheral and bus clocks change as well as the CPU clock. One
[owner report](https://www.pyra-handheld.com/boards/goto/post?id=360437)
describes 188 MHz after a resistor/voltage modification; this is a single
modified unit, not a stock-device guarantee. Another [owner's report](https://pyra-handheld.com/boards/threads/gp32-review-nearly-finished.13628/)
says 166 MHz passed a clock tester but the GPengine game only worked at 156 MHz.
Therefore tester boot, sustained gameplay and audio stability are distinct
evidence levels. Reports of 200 MHz and above need the same distinction and
should not define the default emulated machine.

The guest game already programs its own MPLL. The core derives a run-clock from
those registers in `src/s3c2400.c`, and `src/libretro/libretro.c` asks the CPU to
execute roughly `run_clock / 60` cycles on each `retro_run`. Raising the
**emulated GP32 clock** asks H700 to execute more guest work per real second;
it cannot make a 35 fps host throughput become 60 fps. It may change game
logic, timers, sound pitch or LCD behavior, so an optional high-clock profile
would require independent game-by-game compatibility checks. Raising the
**H700 host frequency** can improve throughput but only within its supported
clock, heat and battery limits; Spruce already exposes CPU modes. No global
RetroArch or device overclock setting is needed to pursue code optimization.

## C23 decision

The build now selects C23 when the compiler and build system advertise it,
and emits an explicit C11 fallback message otherwise. This retains the local
Win64 MinGW GCC 8 path, which lacks C23 support; `GP32EMU_REQUIRE_C23=ON`
rejects a downgrade when required. The H700 Zig 0.13/Clang 18 toolchain builds
the core in C23 mode. C23 is a language standard, not an optimization flag:
[GCC's dialect documentation](https://gcc.gnu.org/onlinedocs/gcc/C-Dialect-Options.html)
and [CMake's `C_STANDARD` documentation](https://cmake.org/cmake/help/latest/prop_tgt/C_STANDARD.html)
describe selecting syntax and semantics, not a faster backend. Compare C11 and
C23 with the same compiler and flags before attributing any speed difference
to the standard itself.

## Probe order

1. Make one repeatable gameplay state per challenging title, starting with Blue
   Angelo and Little Wizard. Record PC/CPSR, cycles, video/PCM hashes, CPU
   frequency, frame times and audio underruns. A game-library result requires
   substantially more than five boot/menu windows.
2. Sample host PCs during the measured window, mapping JIT code, JIT dispatch,
   classified ARM helper, MMU fast/slow paths, LCD scanout and audio separately.
   The resume-4 sampler excludes warmup: generated JIT code and arm_jit_run
   together account for roughly 55% of Blue samples and 77% of Wizard samples.
   These limited SIGPROF samples include hashing and inlined work and are not
   exact cycle percentages. Blue additionally shows framebuffer/read32 costs.
   After fixing repeated RAM callbacks in that framebuffer repair, a separate
   Blue 600/300 sample attributes about 65% to generated code and arm_jit_run
   together, while gp32_get_framebuffer falls to about 1% of samples. The window
   differs from the older Blue profile; use this for priorities, not subtraction.
   A large speedup needs an identified hotspot, not minor compiler flags.
3. Optimize the hottest **safe** path, likely AArch64 JIT dispatch/memory or
   classified instructions, with exact guest state and video/PCM hashes against
   the prior build. Avoid assuming MMU can be bypassed or polling loops skipped
   without per-access stability proof.
   Address attribution now shows that 94.3% of Wizard's non-RAM single
   helpers access GPIO/SmartMedia, and BIOS contributes less than 0.4%.
   Reordering GPIO dispatch gives only 0.82%, so this helper count alone does
   not justify expecting a large game-wide speedup. Profile native execution
   and dispatch costs next; do not replace SmartMedia signal updates with
   register-only memory accesses.
4. Compare `-O3`, LTO/IPO and any CPU tuning in isolated builds using
   interleaved, matched-clock H700 runs. Retain negative results. Then measure
   actual RetroArch frame pacing and audio through Spruce, not only the core
   harness. Consider Spruce's Performance mode as a diagnostic if frequency
   ramping prevents a fair comparison, then restore the user's selection.
5. Add or refine title-specific emulation only when a traced hardware behavior
   proves necessary. Do not trade correctness or sound continuity for a nominal
   60 fps counter. An optional guest overclock belongs after the baseline is
   fast and accurate, with an explicit per-game compatibility label.

The 60-refresh target is a direction for measured work, not an achieved
whole-library guarantee. Long gameplay, battery/thermal behavior, real speaker
sound, and Android runtime acceptance remain open.

## Parallel work and direct execution

GP32 software draws through its ARM CPU into memory; the S3C2400 LCD controller
scans that memory rather than executing a separate GPU command stream. The
current libretro core advances CPU, peripherals and audio on the retro_run
thread. Frontend display/scaling can use a GPU independently of the core.
Offloading LCD conversion does not offload the game's software rendering ARM
instructions. Any worker must also preserve RAM/palette snapshots, LCD register
changes and frame order; reading live guest RAM concurrently is not safe.

The measured JIT/dispatcher hotspot supports prioritizing generated code and
block transitions. Parallel conversion, filters and audio postprocessing need
separate cost measurements, including copies and synchronization, before a
worker queue is justified. Do not add a frame of buffering just to claim
multicore support. The failed resume-6 forwarding experiment is also evidence
that fewer source-level loads do not automatically improve Cortex-A53 speed.
The existing core harness does not call libretro's stage_frame_320x240,
effects or frontend callbacks, so its samples cannot establish a 10% total
offload ceiling or measure removal of that staging pass. Sampled JIT execution
also includes game logic and other guest code, not just software rendering.
Measure the real libretro path before estimating the gain from fusing LCD
conversion with frontend rotation; other frontends consume the current layout.

[Cortex-A53](https://www.arm.com/products/silicon-ip-cpu/cortex-a/cortex-a53)
supports AArch32, but a GP32 binary expects GP32 memory, firmware, privileged
state and devices, not the Linux process environment. A direct-execution
backend would need instruction compatibility handling, trapped device access,
guest timing and self-modifying-code support. It would also require a separate
32-bit runtime instead of jumping from the current AArch64 libretro core into
AArch32 code. This is a research candidate, not an enabled fast path.
[Linux documents](https://www.kernel.org/doc/html/latest/arch/arm64/asymmetric-32bit.html)
that some newer CPUs lack AArch32 execution. Translating ARM920T instructions
to AArch64 remains the portable path on those CPUs; the resulting JIT code
already executes natively on the host.

Direct-execution prototyping must use an explicit allowlist, not assume all
ARMv4T instruction encodings retain their semantics. In particular, code copied
to a different address needs PC-relative fixups, and system/control instructions
must never escape into Linux. The actual H700 probe confirms the unaligned-LDR
semantic difference. Do not assume a userspace process can set privileged
SCTLR alignment controls, reserve an entire 32-bit address space, or map BIOS at
zero under the existing OS policy. A separate 32-bit frontend is another option
besides a helper process; neither integration path is implemented or benchmarked.
AOT translation could avoid runtime compilation for code known in advance, but
does not itself improve the quality of generated code or resolve indirect
branches and self-modifying code. Measure JIT compilation frequency before
choosing a persistent cache or AOT format.

## Accuracy references and open timing questions

[MAME's GP32 driver](https://raw.githubusercontent.com/mamedev/mame/master/src/mame/gamepark/gp32.cpp)
is a useful BSD-3-Clause comparison, not a hardware oracle; its TODO includes
sound clipping/mixing problems. In particular, compare its register-derived
LCD frame timing with our fixed-60-Hz/approximate-blanking model. Record actual
game LCD registers and polling behavior before changing this model; matching
another emulator alone does not establish physical timing accuracy. Likewise,
the current effective run-clock heuristic compensates for instruction-count
CPU execution and is not cycle-accurate ARM920T accounting. Do not remove it
or expose a guest 166/200 MHz override as a performance fix without resolving
CPU/peripheral time relationships. Shared approximations, including PWM compare
versus underflow behavior, need the Samsung manual or hardware evidence.


## Validated translation reuse (2026-10-03)

Guest full I-cache maintenance repeatedly discarded unchanged code. A separate
maintenance epoch now invalidates the dispatch tag without destroying native
storage. On next use, every recorded instruction (including inlined callees) is
verified through a side-effect-free current mapping; changed or unprovable blocks
are translated again. MMU/state/API/recycle events retain full invalidation.
MCR remains a trace terminator, and epoch/generation wrap tests protect reuse.
Eager table and active-slot scans were rejected after real Dungeon regressions.

The retained lazy implementation improves matched-clock H700 ABBA core throughput
by 22.12% in Blue, 13.37% in Dungeon and 5.77% in Wizard, with identical guest
state/video/PCM checks. Wizard still measures 50.493 core fps, so CPU/JIT execution
remains an open target. Validate actual RetroArch pacing and audio before treating
Blue's 66.806 core fps as frontend headroom. PC shares this cache-management
improvement; architecture-specific emitters remain separately constrained.

## resume16 AArch64 JIT MMU-state specialization (2026-10-03)

The resume16 candidate removes the per-access MMU decision from translated
memory operations: CP15 control writes and state loads already invalidate the
block generation, so each block commits to the MMU enable state captured at
translation time instead of reloading CP15 register 1 and branching on its M
bit for every guest access. Memory blocks also pair the x19/x20 callee-save
loads and stores without changing the stack frame or ABI. The TLB-hit tag
check, RAM bounds check and slow/helper paths are unchanged. Matched-clock H700 A/B/B/A comparisons
(baseline bench `cfdfe406f5b3d22f`; candidate `de590d745f667fa7` for Her
Knights and Wizard, later candidate `b8f483fb84edc316` for Mill) measured
these median core throughput changes:

| Fixed scene (warmup + measured) | Baseline median core fps | Candidate median core fps | Change |
| --- | ---: | ---: | ---: |
| Her Knights, first palace battle (1200 + 1200) | 103.4295 | 106.4085 | +2.88% |
| Little Wizard, combat state (1200 + 1200) | 142.9080 | 147.4280 | +3.16% |
| Mill, room->village progression (0 + 1700) | 124.8520 | 130.0965 | +4.20% |

Every run in the three primed comparisons matched all seven CPU/video/PCM
fields (cycles, pc, cpsr, clock, audio_frames, video_hash, audio_hash),
reported no disqualifiers or clock-monitor errors, and every measured
frequency sample read 1512 MHz with the governor left on `conservative`.
These are uncapped core throughput figures for fixed scenes, not displayed
frame rates or all-game minimums, and they must not be multiplied with the
other experimental gains above.

The earlier non-primed Her Knights comparison is disqualified: its cold first
baseline was still ramping (observed 1416 and 1512 MHz), so that comparison
cannot establish a speed gain. The retained protocol passes `--prime`, one unscored
invocation of the same workload before the A/B/B/A order, which allowed the
device to reach a steady 1.512 GHz in these runs without changing the governor
or relaxing the measured-clock qualification; the Mill scene's unscored prime run itself
measured 78.4 fps before the scored runs settled at 124.84-130.38 fps.

The Mill window is a real progression rather than a combat loop: from the
resume13 in-room state it walks out behind the ginkgo tree into "ROOT Village /
뿌리마을" and advances an NPC dialogue line by line with A. The canonical
command re-ran verbatim with all seven dump frames byte-identical and the end
state reloading in the village; the previous resume13 movement frames also
reproduce byte-identically from the same runner. Coverage is one title, one
room, one transition and one dialogue, with no combat, audio, save-game or
full-game completion claim; the ROM, BIOS and states remain external.

Evidence: `F:/GP32/results/resume16-mmu-her-primed-abba.json`,
`F:/GP32/results/resume16-mmu-wizard-primed-abba.json`,
`F:/GP32/results/resume16-mmu-mill-primed-abba.json` (the disqualified cold
comparison is `F:/GP32/results/resume16-mmu-her-abba.json`) and
`F:/GP32/results/resume16-mill/README.md`. Final integrated CPU fixes and
frontend/device validation are recorded separately in [portability progress](PORTABILITY_PROGRESS.md).


## resume17: shifted-register instruction folding

AArch64 shifted-register ALU instructions now absorb the separate shift for
non-flag-setting logical operations and supported ADD/SUB/CMP/CMN operands.
RRX, encoded LSR/ASR #32, reversed-subtraction operands, ADC/SBC/RSC, and logical
shifter-carry updates retain the explicit path. Guest state is still committed
after each instruction; no register cache or deferred flags were introduced.

At the same observed 1.512 GHz, primed ABBA Her Knights (1200/1200) measured
106.172 -> 106.7275 core fps (+0.52%); Korean Little Wizard combat (1200/1200)
measured 147.178 -> 147.9575 (+0.53%). Both comparisons preserve all seven
CPU/video/PCM fields. These small scene-specific changes do not imply a large
whole-game gain or a frame-rate floor. The candidate was
`0081c5132f78b190133b8e80c680eeef3e506115dec67316865ecdb359907d7d`, baseline
`3f4197cb57707d63cade51dbf246e2be2bfe1cbbdf9d0afb151de7782759fdb9`.
Evidence: `F:/GP32/results/resume17-shift-{her,wizard}-abba.json` and
`resume17-shift-jit-h700.json`. The native H700 differential includes flags
and per-instruction result snapshots for all immediate-shift forms, including
zero-field boundaries and both input carries; 39,128 events passed with four
expected fallbacks.

The final integrated resume17 build also measured GP Fight's Korean classroom
match (0 warmup / 2400 measured frames, scripted movement). Primed ABBA at an
observed steady 1512 MHz gave baseline 168.072/168.218 and candidate
171.704/170.837 core fps: medians 168.145 -> 171.2705 (+1.86%). All seven
CPU/video/PCM fields matched, with all 52 clock samples steady and the governor
unchanged. This comparison includes the CPU correctness fixes as well as shift
folding; it does not isolate a single optimization or establish displayed fps.
The final candidate bench SHA-256 is
`36020feaedb425b2ce232d41b171c15142cfba4f006956032db8d1ef103fc81f`.
Evidence: `F:/GP32/results/resume17-gpfight-abba.json`.

## resume18: reuse adjacent arithmetic condition flags

After an unconditional native arithmetic instruction writes all four guest
flags, the next conditional instruction can branch on the already matching
AArch64 NZCV instead of loading CPSR and writing NZCV again. CPSR and guest
registers are still committed per instruction. Memory guards, helpers, logical
partial-flag writes, register-shift paths and predicated joins conservatively
invalidate this compile-time fact; nothing is cached across native blocks.

Qualified primed ABBA on the Korean Her Knights combat script (1200/1200,
observed 1512 MHz) measured baseline 106.784/106.724 and candidate
110.172/110.639 core fps. The medians are 106.754 -> 110.4055 (+3.42%), with
all seven CPU/video/PCM fields unchanged and all 42 clock samples steady.
This measures the core's execution headroom in this scene, not game animation
rate or end-to-end input latency. Baseline SHA-256:
`36020feaedb425b2ce232d41b171c15142cfba4f006956032db8d1ef103fc81f`;
candidate: `12739009cc093f7e7d00d5d808a40986d4da37505957568c0ffb6f41000f870e`.
Evidence: `F:/GP32/results/resume18-flags-her-abba.json`.

The previously slower Little Wizard selection workload (cold boot, 2400 warmup
then 300 measured frames) measured 58.047/58.280 baseline and 60.274/60.364
candidate core fps. Medians 58.1635 -> 60.319 give +3.71%. All seven fields
matched and all 20 clock samples were 1512 MHz. Crossing 60 in this core-only
window leaves little frontend headroom; it is not evidence of sustained 60-fps
RetroArch presentation. Evidence:
`F:/GP32/results/resume18-flags-wizard-menu-abba.json`.

## Rejected follow-ups (resume19)

Extending the adjacent-condition optimization to reuse only N/Z after logical
instructions passed the existing native H700 differential (42,769 events,
four expected fallbacks). However, qualified primed ABBA on Her Knights combat
gave 110.424 -> 110.3385 core fps (-0.08%), with all seven fields exact and all
40 clock samples at 1512 MHz. This provides no useful gain in that scene. The
extra flag-mask tracking was removed; no speed claim is made for unmeasured
scenes. Candidate `21315d73618f4ac4776d9dc3fcdec00b08ccb0411b8d9b2bd4d7c4ce8d058a63`
and source `resume19-rejected-nz.inc` remain outside Git for reproduction.
Evidence: `F:/GP32/results/resume19-nz-{jit-h700,her-abba}.json`.

The video-effects worker also left production code unchanged. Removing a
redundant source-alpha mask preserved visible output, but its initial apparent
host speedup disappeared under finer interleaving: the final x86 measurements
varied around parity across interpolation/LCD-persistence modes and image sets.
The first memcpy timing floor was eliminated by the compiler and is invalid;
the corrected measurement retains its output. These are host results, not H700
cost estimates. The effects are disabled in the current default frontend path,
so this candidate cannot improve that path. No device benchmark or deployment
was justified for this candidate. Artifacts:
`F:/GP32/results/resume18-video/REPORT.md`, `final-o3.txt`, and `cand_src.c`.


## resume20: smaller AArch64 leaf frames

Native ALU/branch-only traces omit the x29/x30 frame and save just x19 with a
16-byte-aligned SP. Memory/helper traces keep the old frame; any unexpected
helper in a leaf candidate prevents native publication. Per-instruction guest
state and cycle accounting are unchanged.

Primed H700 ABBA at an observed 1512 MHz, with the governor unchanged:

| Scene | Baseline core fps | Candidate core fps | Median change |
| --- | --- | --- | --- |
| Her Knights Korean combat, 1200 warmup / 1200 measured | 110.219 / 110.189 | 113.032 / 113.101 | 110.204 -> 113.0665 (+2.60%) |
| Little Wizard selection, 2400 warmup / 300 measured | 60.595 / 60.361 | 60.773 / 61.255 | 60.478 -> 61.014 (+0.89%) |

All seven CPU/video/PCM fields match. The Her comparison has 40 steady-clock
samples and Wizard has 20. Wizard's gain is small and the menu window still
has little frontend headroom; these are core throughput results, not minimum
displayed fps or physical audio/input-latency claims.

Baseline bench SHA-256:
`12739009cc093f7e7d00d5d808a40986d4da37505957568c0ffb6f41000f870e`;
candidate:
`8cb9b887fdcf73bb5d11b5ef7eee28b6feec0fed1c128909d2fb114468e12d85`.
Evidence: `F:/GP32/results/resume20-leaf-her-abba.json` and
`F:/GP32/results/resume20-leaf-wizard-menu-abba.json`.


## resume21: reduce repeated native-dispatch checks

A measured-window SIGPROF sample of the current Wizard selection scene yielded
485 samples: generated JIT code 57.11%, `arm_jit_run` 24.12%, the benchmark main
(including hashing) 8.04%, and the classified helper 4.54%. These are coarse
sample proportions, not exact cycle costs. Profiling counters for the same
2400/300 workload count 27,405,276 native calls / 270,050,092 native instructions.
Evidence: `F:/GP32/results/resume21-sample-summary.json` and
`resume21-wizard-profile.json`.

Clang had inlined translation/native emission into dispatch: `arm_jit_run` was
35,904 code bytes with a 66,048-byte reserved stack frame. Outlining translation
and the portable fallback reduced the hot path (now inlined into `arm920t_run`)
to a 240-byte frame. The large emitter scratch still exists when translation
actually occurs; the old hot path did not clear/copy that entire frame per call.
Outlining alone measured 61.344 -> 61.408 core fps (+0.10%) in Wizard, so it is
not independently claimed as a meaningful speedup. Disassembly and the isolated
negative/marginal comparison are preserved under `resume21-*-asm.txt` and
`resume21-outline-wizard-menu-abba.json`.

The final candidate also relies on translation's existing publication contract:
a nonnull native pointer in a valid cached block already means compilation
succeeded, and stable-read polling backedges never receive native code. Bus
callbacks are copied once at CPU creation. Dispatch therefore avoids redundant
native_ok/poll eligibility checks and computes read-stability bookkeeping only
for portable execution. Cache tags/generations, remaining budgets, IRQ checks
and runtime polling fixed-point checks are unchanged.

Qualified primed H700 ABBA, observed 1512 MHz with the governor unchanged:

| Scene | Baseline core fps | Candidate core fps | Median change |
| --- | --- | --- | --- |
| Wizard selection, 2400 warmup / 300 measured | 61.472 / 61.472 | 64.819 / 63.893 | 61.472 -> 64.356 (+4.69%) |
| Her Knights Korea combat, 1200 / 1200 | 112.721 / 113.182 | 119.642 / 119.518 | 112.9515 -> 119.580 (+5.87%) |

All seven CPU/video/PCM fields match. Wizard has 19 matching frequency samples,
Her has 40. Baseline bench:
`8cb9b887fdcf73bb5d11b5ef7eee28b6feec0fed1c128909d2fb114468e12d85`;
CPU candidate:
`10d5eae849ccf002c92a6e9f3a8230e2bbd0bfb2f8da95b1b1bc4f9feeaba3f8`.
These comparisons exclude the separate experimental audio/video candidates.
Evidence: `F:/GP32/results/resume21-dispatch-{wizard-menu,her}-abba.json`.

The Windows GP Fight replay was also exact for the outlining-only candidate,
but its unmonitored short workstation runs are not a qualified performance
comparison (`resume21-outline-pc-gpfight.json`). No PC speedup is claimed.


## resume22: isolate the late Wizard slowdown

The previous 56.74 fps screenshot did not identify which part of the match was
slow. An external diagnostic wrapper now times each `retro_run`, the actual
`gp32_run_cycles` call, video/audio callbacks and intervals between runs. It
links the unchanged resume21 core; timings include scheduling and callback
waits and do not measure physical button-to-display latency.

In the Korean Wizard saved-state replay, frames 600-1800 spend about 2.5 ms on
core execution per frame (p99 below 6.9 ms). Most of the remaining time is in
the frontend's video callback, consistent with normal display pacing. The last
600 frames, including the loss screen, rise to 11.29 ms mean / 16.42 ms median /
17.59 ms p95 core time. In that window 183 frames exceed 16.67 ms in core work
alone. The late audio callback averages 0.206 ms; its maximum is 0.503 ms.
Thus average combat throughput conceals a late guest-CPU bottleneck.

Measured-window SIGPROF on that late 2100-warmup / 300-frame workload collected
584 samples: generated JIT 50.68%, `arm920t_run` 33.73%, benchmark hashing/main
10.10%, LCD 3.25%. The full 1200/1200 combat window has a different mix (JIT
39.90%, dispatcher 19.70%, hashing 23.17%, LCD 7.13%). Sampling is approximate
and instrumented FPS is not a candidate speed comparison.

The timing run completed 2400 frames, accepted all 1,762,933 stereo frames,
and reported zero ALSA errors/recoveries. Settings were unchanged and MainUI
returned. Diagnostic probes remain outside the repository and installed core.
Evidence: `F:/GP32/results/resume22-timing-summary.json`,
`resume22-timing-runtime-verified.json`, `resume22-sample-summary.json`.

Outlining `arm_jit_fetch_unchanged` alone reduced the dispatcher's reserved
stack from 240 to 224 bytes, but a primed, 1512 MHz ABBA measured only
69.7685 -> 70.0385 core fps (+0.39%) in the late window. All seven exactness
fields matched. This marginal candidate remains outside production at
`F:/GP32/results/resume22-dispatch/`; comparison evidence is
`resume22-dispatch-wizard-late-abba.json`. Its unscored prime was 52.127 fps
and ended at 1320 MHz, motivating live runtime frequency observation before
attributing the prior frontend slowdown entirely to core code.

A follow-up runtime read `scaling_cur_freq` every 60 frames without changing
the governor. All 40 observations were 1512 MHz, including the late slowdown.
Late core time remained about 15.7-16.0 ms per frame, with frontend intervals
about 17.5-17.7 ms. This falsifies governor downclocking as the explanation for
that observed runtime. Standalone throughput and real frontend timing must not
be conflated: scheduling, rendering and diagnostic OSD overhead remain possible
contributors. Evidence: `F:/GP32/results/resume22-clock-summary.json` and
`resume22-clock-runtime-verified.json`.

An attempted no-OSD replay appended false values to the temporary config,
but the captured image still shows statistics. The duplicate-key edit did not
establish that OSD was disabled, so this comparison is invalid for ruling out
OSD overhead. Its timing artifact is retained as another instrumented replay,
not an OSD A/B result (`resume22-clean.csv`, `resume22-clean-runtime.png`).
A future no-OSD run must replace all existing keys and verify the image.

The A64 address-folding candidate subsequently measured 69.8835 -> 70.6575 core
fps (+1.11%) in the late window under primed ABBA, with 16 matched 1512 MHz
samples and all seven CPU/video/PCM fields identical. Baseline benchmark SHA:
`834553a87ff73c75d3cace98520263c704aba8f8c864eb9a29099952d2b3648b`;
candidate: `e368cf784a9de277e93d640673e016b7522392562bb9fcd12e9f86ef448107d4`.
Evidence: `F:/GP32/results/resume22-a64-wizard-late-abba.json`.

Her Knights Korean combat also improved: baseline 119.672/119.645, candidate
120.981/121.087 core fps (medians 119.6585 -> 121.034, +1.15%). All seven fields
match, with 38 steady 1512 MHz samples. Evidence:
`F:/GP32/results/resume22-a64-her-abba.json`. These modest, repeatable core
throughput gains are additional to resume21; they do not establish minimum
displayed FPS across the library.

## resume47: stable timer polls with idempotent RAM stores

Disassembly of Little Wizard Korea's measured hot entry `0c002f04` identifies
a timer-wait loop: call a balanced timer leaf, store the timestamp, subtract
the origin, call a register-only absolute-value leaf, then compare a deadline.
The earlier stable-poll classifier rejected the ordinary STR and the decoded
register-leaf return. Repeated computation was consuming most of this window.

The shared CPU path now admits immediate pre-indexed word STR without
writeback or PC data, provided every observed store is aligned, maps to
ordinary writable RAM through fastmem, and already contains the value being
stored. A decoded register-only leaf return can remain in the poll prefix.
The existing full-register/CPSR fixed point, stable-read checks, run budget,
IRQ and invalidation fences still apply. Each skipped complete repetition is
charged its original guest instruction count; peripheral time is unchanged.
Writing then restoring RAM is not enough: every intermediate store is checked.

Native H700 differential and Windows/H700 stable-poll regressions passed.
New cases exercise changing timer values between runs, budgets inside the
two leaf functions, deadline exit, intermediate RAM mutation, MMIO writes,
and readable fastmem that refuses writes. Android ARM64/ARMv7 and Windows/H700
libretro builds succeeded.

Qualified H700 ABBA, all measured clock samples at 1,512 MHz:

| Scene | Baseline core fps | Candidate core fps | Median ratio |
| --- | --- | --- | --- |
| Little Wizard Korea, late 2100/300 | 112.983 / 113.122 | 400.395 / 399.529 | 3.5378x |
| Her Knights Korea combat, 1200/1200 | 122.485 / 122.458 | 121.935 / 122.724 | 0.9988x |
| Tomak Korea shooting, 300/600 | 111.159 / 112.071 | 111.619 / 111.186 | 0.9981x |

All seven CPU/video/PCM fields match in every comparison. Her Knights and
Tomak remain within observed variation; no gain is claimed for them. These
are core throughput windows, not displayed fps or complete-game guarantees.
The candidate's short Wizard window required a private 0.2-second clock
sampling interval instead of the normal one second; all qualification rules
remain unchanged. The initial one-prime Wizard sequence included 1,416 MHz
and is retained as disqualified. Two recorded unscored primes stabilized the
second sequence, with 28 measured samples and at least three per invocation.
No governor or device setting was changed.

Evidence: `F:/GP32/results/resume47-poll/`, especially `wizard-warm.json`,
`her.json`, `tomak.json`, `h700-jit.json` and `h700-poll.json`.
Baseline benchmark SHA-256:
`985f263e06f5501ecd69519329a959eafd61313f220a23d3890dce1d982bdab3`;
candidate: `5df7815fc7a2551e0b9ac3058dba4336f505c70eebbded05078301f688ce1d8e`.

Before this change, a separate A64 logical-immediate folding candidate passed
the native differential but showed no useful gain against the warm baseline
in Wizard or Tomak. Both comparison sequences were clock-disqualified, and
the warm endpoints were effectively equal or worse. That candidate and its
tests were reverted and archived under `resume47-logical/`; none was installed.

A private real-RetroArch replay then completed 2,401 runs and returned to
MainUI. Every row's guest cycle/PC, source audio frame/nonzero counts and IIS
state matched the earlier identical replay. All 1,763,356 offered audio frames
were accepted, with no partial/zero callback returns or pending frames; ALSA
reported no write errors or recovery. This frontend has no explicit FIFO
padding trace, so zero-valued samples cannot establish absence of padding.

The last 300 runs had core time mean 1.397 ms, p99 5.238 ms, max 7.516 ms;
frontend callback intervals had p99 17.067 ms and max 17.205 ms, none above
20 ms. Synchronous video includes vsync wait. This late window includes the
game's IIS-disabled state, so it is not a continuous active-audio stress test.
These measurements do not establish physical input latency or panel refresh.
All protected installed settings and binaries remained unchanged during the
private run. Evidence: `resume47-poll/runtime-analysis.json` and
`resume47-poll-runtime-verified.json` under the local results directory.

## resume50: AArch64 PC-relative address folding

For an immediate PC-relative transfer without writeback, the effective
address is determined by the decoded PC and offset. The A64 emitter now
folds its address arithmetic and word alignment during translation. An
aligned word load also needs no rotate; an unaligned one uses the known
immediate rotation. Actual memory contents, MMU/RAM validation, slow helper
fallbacks and guest instruction accounting remain dynamic and unchanged.

Native H700 differential passed (9,115 JIT events / 21 fallbacks), including
new positive/negative literal offsets, all four byte lanes, stores and changed
literal data between executions. The Windows test target compiles, and H700
and Android ARM64 libretro builds pass. This changes only the A64 backend.

Qualified ABBA at observed 1,512 MHz, with all seven CPU/video/PCM fields exact:

| Korea scene | Baseline core fps | Candidate core fps | Median ratio |
| --- | --- | --- | --- |
| Her Knights combat, warm 1200 / measured 1200 | 121.962 / 121.954 | 122.560 / 122.466 | 1.0046x |
| Tomak shooting, warm 300 / measured 600 | 110.901 / 110.636 | 112.057 / 111.298 | 1.0082x |

These are small bounded throughput improvements (0.46% / 0.82%), not minimum
displayed frame rates or whole-game gains. Both candidate runs exceeded both
baseline runs in each scene. The same private two-prime/0.2-second sampling
runner as resume47 was used, with qualification rules unchanged and no
governor changes. Evidence: `F:/GP32/results/resume50-cpu/`.
Baseline benchmark SHA-256:
`9cdd288947d4d6e3a150322f43dde9b6a0e229e0fcd4321622a7ca8b1ae32834`;
candidate: `1dcac1723555a9bd0ff02c3e81e4e3433917c94eadb057a07f788da9ff1bacf4`.

The parallel LCD review found a possible palette expansion in the indexed
fallback renderer. The measured scenes use the already optimized contiguous
renderer, so that change was not implemented or presented as a game speedup.
Her Knights disassembly also identifies a multi-block timer wait with a
nested call and single-transfer stack wrappers; the existing bounded leaf
poll accelerator does not cover that complete cycle. This is a future
investigation target, not permission to skip those instructions without
per-access stability and full-state proof.
