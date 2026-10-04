# GP32 on H700: performance research and next probes (2026-10-03)

## Target and measurement

The practical target is **60 emulated video refreshes per real second**, with
uninterrupted PCM and correct game timing, through SpruceOS/RetroArch on an H700
class device. This does not promise that a game originally animating at 30 fps
will acquire 60 unique animation frames. A headless benchmark above 60 fps is
necessary headroom, not sufficient proof of 60 fps in RetroArch.

The current stretch goal is three times the throughput of a **fixed slow
workload on the pre-change core**, not three times the emulated GP32 clock.
Preserve normal guest speed and spend the headroom on combat, transitions and
audio. Acceptance remains sustained real-time presentation, no host-caused
audio starvation and bounded frame/input latency; a 75--90 core-fps heavy
window is an initial headroom target, not a whole-library guarantee.

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

## resume51: bounded nested framed calls

The Her Knights wait wrapper saves LR with STR, calls a five-instruction
clock getter, then returns through LDR PC. The translator now recognizes the
exact single-transfer push/pop encodings alongside STM/LDM and can flatten
one nested framed-call level inside an STR-LR outer wrapper. Existing STM-LR
wrappers retain their non-nested boundary. The outer function is bounded to
16 source instructions, a child to eight, and the combined callee to 32 decoded ops.
Ordinary non-nested leaves retain their eight-instruction limit.

Every instruction still retires, including BL links and real stack transfers.
Speculative callee fetches use the existing non-mutating mapped-code peek;
missing mappings, page crossings or unsupported control flow reject inlining.
Recorded PCs remain part of cache-epoch revalidation. Nested frames do not
qualify as a stable-poll proof, so this does not skip the complete wait loop.

A64 single-transfer returns can continue only when the actual aligned loaded
PC matches the recorded successor. Modified stack returns exit immediately,
as with the existing block-transfer return guard. Checked helper paths retain
their PC/status/IRQ/cache fences. Windows differential checks and native H700
short-budget checks cover nested frames and aliasing of the outer saved return
against instruction-by-instruction execution.

The first wider candidate improved Her Knights 5.82% but reduced Tomak 0.67%.
Keeping the old non-nested length limit retained Her's 5.93% gain but did not
remove Tomak's 0.70% loss. Both rejected intermediate measurement sets are
preserved in `F:/GP32/results/resume51-inline/` (`wide-*`, `narrow-*`). The
matching-return candidate reached Her +11.00% but Tomak -0.81%; its
`return-guard-*` records are also retained. Limiting nested expansion to STR-LR
outer wrappers removed that measured Tomak slowdown. The final candidate
retains the A64 loaded-return guard; separate final records determine claims.

Qualified final ABBA at observed 1,512 MHz (conservative governor unchanged):

| Korea scene | Baseline core fps | Candidate core fps | Median ratio |
| --- | --- | --- | --- |
| Her Knights combat, warm 1200 / measured 1200 | 122.391 / 122.476 | 135.97 / 136.808 | 1.1140x |
| Tomak shooting, warm 300 / measured 600 | 111.052 / 111.233 | 112.101 / 111.08 | 1.0040x |

Her improved 11.40%. Tomak is essentially unchanged (+0.40%, overlapping run
ranges); no broad speedup is claimed for it. All seven CPU/video/PCM fields
match. These are core-throughput measurements, not displayed-FPS minima or
evidence of continuous clean speaker output. Input scheduling, guest clocks,
audio generation and global frontend settings are unchanged.

Baseline benchmark SHA-256:
`1dcac1723555a9bd0ff02c3e81e4e3433917c94eadb057a07f788da9ff1bacf4`;
final candidate: `1eb6ba64c272c0d431d1e333d4b092c94cf33241f5c850899ccfec97067ff8e0`.

Final native H700 differential passed (9,404 JIT events / 21 fallbacks), as
did the existing stable-poll equivalence suite. Windows nested-call checks
and Windows/H700/Android ARM64/ARMv7 libretro builds pass. No Android runtime
or new speaker/input-latency measurement was performed in this step.

## resume52: conditional entry into a call-containing loop

A same-page forward conditional branch can now follow its taken target when
that short source segment contains an unconditional call and a backedge to
the current block entry. The lookup is bounded to eight source instructions
and uses non-mutating code peeks. NV/AL branches, page crossings, already
recorded targets and a missing successor slot retain the existing layout.

Both native backends explicitly exit to the original PC+4 when the branch
condition fails, counting that branch exactly once. Portable execution uses
the same real branch and expected-successor check. AArch64 can keep this
trace on its existing budgeted self-loop path; loaded stack returns and
helper/IRQ/cache fences remain active. No guest instruction is skipped by
the native loop and no new emulated-clock or audio policy is introduced.

The unrestricted forward-loop prototype improved Her Knights but reduced
Tomak throughput 13.39%. It was rejected. Plain loops keep their existing
layout: joining them can expose a portable polling candidate where native
execution was already effective. The final rule targets call-containing
loops instead. Rejected records are `plain-loop-*` under
`F:/GP32/results/resume52-branch/`; final qualification records are separate.

Final qualified ABBA at observed 1,512 MHz, all seven CPU/video/PCM fields exact:

| Korea scene | Baseline core fps | Candidate core fps | Median ratio |
| --- | --- | --- | --- |
| Her Knights, warm 1200 / measured 1200 | 136.686 / 135.879 | 153.194 / 154.134 | 1.1275x |
| Tomak, warm 300 / measured 600 | 111.531 / 111.084 | 110.596 / 111.065 | 0.9957x |
| Little Wizard, warm 2100 / measured 300 | 399.627 / 401.153 | 399.919 / 399.145 | 0.9979x |

Her improved 12.75% relative to resume51. Tomak (-0.43%) and Wizard (-0.21%)
are near parity; they are not claimed as speedups. The latter retains its
previous large stable-poll gain. These bounded scene measurements do not
prove whole-game frame rates or uninterrupted speaker output. Governor,
guest clocks, audio generation and input scheduling are unchanged.

Measured baseline SHA-256:
`1eb6ba64c272c0d431d1e333d4b092c94cf33241f5c850899ccfec97067ff8e0`;
candidate: `40e0f58dc95e202584c10934561bae78b18d7a570211eed6c9f4d237b78da695`.
A comment-only rebuild changes metadata and the binary hash to
`b33c4f0476b35c91d5c98521bb507617511faaffa468423836ed3eec75bfa5ce`.
All runtime-allocated ELF sections, addresses, flags and sizes are identical
(excluding note metadata), recorded in `runtime-section-equivalence.json`.

Native H700 full differential passed (9,510 JIT events / 21 fallbacks), as did
stable-poll equivalence. The new forward-loop cases cover taken/untaken paths,
real framed calls, native-plus-partial budgets, flag-consuming exits and
post-indexed sentinel reads against single-step execution. The focused cases
also pass on Windows. Windows/H700/Android ARM64/ARMv7 cores build; no Android
runtime or new physical audio/input-latency result is implied.

## resume53: real frontend audio delivery and software input timing

The resume52 core was replayed through actual H700 RetroArch twice: once with
the installed production GP32 frontend and once with a private frontend that
logs FIFO padding. Both use the production GP32 profile (synchronous video,
rate-control delta 0.02), a private save/config directory and the same Korean
Her Knights combat state and scripted input. Each run completes 2,401 calls
(one boot call followed by 2,400 state-replay frames). The diagnostic frontend's
experimental threaded completion path is inactive because video is synchronous;
it was not installed. The normal installed core remains SHA-256
`9abfb0e44d4aafe280871ce50f0948aa1a89a4c37cb39a0e7aae6be6549b69f3`.

Every replay frame matches between the two runs in guest cycles, PC, source
audio frame count, nonzero-source count and IISCON. The production run also
matches the resume43 production replay in these fields. Final RGB screenshots
match between the two new runs. Both deliver all 1,764,180 offered audio frames
with no partial/zero callback returns or retained queue backlog. ALSA reports
no write errors or recovery calls.

The explicit diagnostic FIFO trace contains **zero padded frames** in the
48 kHz output sample window [96,000, 1,872,000), corresponding to seconds 2-39.
There are 74,895 padded frames outside that window; startup/shutdown padding
is not presented as missing gameplay audio. This checks inserted padding,
not just PCM zeros, which can legitimately occur in the source waveform.

Late 1,200 frames, observed CPU clock 1,512 MHz throughout:

| Metric | Production mean / p99 / max (ms) | Diagnostic mean / p99 / max (ms) |
| --- | --- | --- |
| Core execution | 5.660 / 7.282 / 12.479 | 5.667 / 7.516 / 7.922 |
| Frame interval | 16.787 / 17.154 / 21.117 | 16.787 / 17.133 / 18.275 |
| Input latch to synchronous video callback return | Not instrumented | 16.087 / 16.374 / 16.791 |

All late-window rows have enabled, nonzero source audio. The production run
has one frame interval above 20 ms; this result does not claim a universal
60 fps floor. Compared with the earlier resume43 run of the same scene, mean
core time falls from 6.998 to 5.660 ms and p99 from 10.171 to 7.282 ms, while
display pacing remains frontend-limited. This is not a new controlled ABBA
performance comparison.

Input timestamps bracket the core's button latch and return from the video
callback. They exclude physical button sampling before the latch and actual
panel scanout afterward. The existing Windows libretro input regression also
passes press/release, save/load, disconnect/reconnect and bitmask/fallback
paths with JIT off/on, showing GPIO observes input before same-run video.
No physical button-to-photon or speaker recording was performed.

Protected frontend settings, installed core and original frontend hashes are
unchanged; MainUI returns and the launch queue is empty. Evidence is in
`F:/GP32/results/resume53-runtime/production-analysis.json`,
`F:/GP32/results/resume53-diag/integrity.json`, and their companion raw CSV,
runtime verification, delivery and ALSA records. These results validate the
existing code; this step introduces no new runtime change.

## resume54: rejected nonconverging-poll native promotion

Four implemented prototypes investigated the native suppression of polling
candidates whose registers continue changing. All qualified Her Knights and
Tomak ABBA runs preserve all seven CPU/video/PCM fields at observed 1,512 MHz,
but none improves either measured scene:

| Prototype | Her core-throughput change | Tomak core-throughput change |
| --- | --- | --- |
| Three portable probes, then native for the remaining timeslice | -0.47% | -0.79% |
| Same adaptive policy plus plain forward-loop stitching | -1.07% | -2.07% |
| Static counter exclusion plus plain forward-loop stitching | -2.94% | -3.63% |
| Static counter exclusion with original trace layout | -2.92% | -3.09% |

The adaptive prototype rechecks each timeslice, preserves the existing
fixed-point proof and caches failed native compilation. Its H700 full JIT
differential and polling suite pass. A stronger fixture subsequently exposed
that disabling RAM fastmem also disables JIT in `arm920t_set_jit`; the fixture
was corrected to retain both memory bases and count a mock stable device word.
The Windows test then confirms actual native allocation for a changing loop
and later recovery of the stable-read shortcut. The corrected broad adaptive
fixture also passes on H700. These correctness results do not imply speedup.

The static alternative detects unconditional nonzero ADD/SUB to a register
with no other writes in the polling prefix. It adds no per-dispatch probes.
Its Windows regression demonstrates that the old core leaves the counter
portable and that the prototype compiles it, while preserving a genuinely
stable increment-then-restore loop. Nevertheless the real-scene results above
reject this policy too. No static-prototype full H700 JIT result is claimed.

All prototype source and private binaries are retained under
`F:/GP32/results/resume54-poll/`, with `rejected.json` linking the exact
candidate hashes and measurements. None was installed. Runtime source was
restored to the pre-experiment revision; rebuilding restores the identical
H700 core SHA-256 `9abfb0e44d4aafe280871ce50f0948aa1a89a4c37cb39a0e7aae6be6549b69f3`
and benchmark SHA-256 `b33c4f0476b35c91d5c98521bb507617511faaffa468423836ed3eec75bfa5ce`.
Installed core and protected settings also match their prior hashes.

Do not promote more polling candidates merely because they fail to converge:
native eligibility alone is not a performance result. Further work on this
path first needs attribution of native/helper costs in the actual affected
blocks. The broad forward-loop restriction remains in production.

## resume55: reuse the AArch64 loop's control-field address

A current measured-window census retains exact replay outputs. Her Knights
executes 571,209,279 guest instructions through 11,315,274 native block calls;
only 946,831 operations use classified helpers. Tomak has 404,170,536 guest
instructions through native blocks, 15,050,388 native calls and 1,510,416 helper operations. These
are workload counts, not timings. They support optimizing already-native
execution rather than broadly increasing native eligibility.

CPU control fields lie after the large TLB arrays. Previously each native
self-loop check repeatedly materialized their distant addresses. Eligible
loop traces now initialize x25 to the control-field base once in the prologue
and use scaled immediate offsets for reachable fields. x25 already had a
callee-saved stack slot in loop traces; no extra save/restore is introduced.
Out-of-range or misaligned offsets retain the existing address path.

Every IRQ/FIQ, generation and cache-epoch value is still loaded at each guard.
Guest register commits, helper exit checks, budgets and loop boundaries are
unchanged. This caches only a host address, not guest state or guard results.
Non-loop traces and other CPU backends retain their previous paths.

Qualified H700 ABBA at observed 1,512 MHz, all seven CPU/video/PCM fields exact:

| Korea scene | Baseline core fps | Candidate core fps | Median change |
| --- | --- | --- | --- |
| Her Knights, warm 1200 / measured 1200 | 152.895 / 153.325 | 158.690 / 159.435 | +3.89% |
| Tomak, warm 300 / measured 600 | 111.718 / 111.589 | 112.618 / 113.241 | +1.14% |
| Little Wizard, warm 2100 / measured 300 | 398.477 / 400.012 | 400.739 / 401.405 | +0.46% |

Wizard remains near parity; these are bounded core-throughput observations,
not whole-game/display-FPS or new speaker/input-latency measurements. H700
full native differential passes (9,510 JIT events / 21 fallbacks), including
callback, IRQ, cache/SMC and partial-budget fences; stable-poll equivalence
also passes. Android ARM64 builds successfully; no Android runtime is claimed.

Evidence: `F:/GP32/results/resume55-profile/` contains the census, qualified
comparisons, test results and guarded deployment record. Baseline benchmark
SHA-256 is `b33c4f0476b35c91d5c98521bb507617511faaffa468423836ed3eec75bfa5ce`;
candidate is `42c3d697a074917074e630bd91dea2739ef29347c6d50f24ed69c1d9822bd515`.
Installed core is `3a0fb70ae46c3bcefc618e5a1100295130da291e26318f40ae3624571059f15a`;
the previous core is backed up as `resume55-installed-core-before.so` under
the device's `gp32-dev` directory. Protected settings are unchanged.

## resume56: inactive interrupt-pair shortcut

AArch64 native self-loop edges now load the adjacent 32-bit IRQ/FIQ lines
as one 64-bit pair. When both are zero, they skip the independent CPSR-mask
checks. Any nonzero bit, including a noncanonical value restored from a
state, retains the original IRQ and FIQ checks. A compile-time assertion
requires the expected field sizes and adjacency. Budget, generation and
cache fences remain unchanged; no interrupt state is cached.

Qualified H700 ABBA at observed 1,512 MHz preserves all seven CPU/video/PCM
fields exactly:

| Korea scene | Baseline core fps | Candidate core fps | Median change |
| --- | --- | --- | --- |
| Her Knights, warm 1200 / measured 1200 | 159.698 / 159.647 | 160.328 / 160.452 | +0.45% |
| Tomak, warm 300 / measured 600 | 112.587 / 112.721 | 113.761 / 113.081 | +0.68% |
| Little Wizard, warm 2100 / measured 300 | 404.352 / 402.205 | 402.443 / 405.771 | +0.21% |

These sub-percent observations are near parity, not evidence of a robust
speedup or higher display frame rate. The shortcut reduces the emitted work
on the inactive path. H700 full native differential passes (9,510 events /
21 fallbacks), as does stable-poll equivalence. Android ARM64 builds; no new
Android runtime, physical audio or input-latency result is claimed.

Evidence: `F:/GP32/results/resume56-edge/` contains the qualified comparisons,
test/build logs and guarded deployment record. Baseline benchmark SHA-256 is
`42c3d697a074917074e630bd91dea2739ef29347c6d50f24ed69c1d9822bd515`;
candidate is `1c3ae96b1ba50b21585eb5f20d9e894ad96a6cead23cdc4ae7e81ec0fb4cf41d`.
Installed core is `4d2e93cd8515f378e42c59ce21b94c9ac7b460331b3bcc8e517b49898db8c7ef`.
The prior core is backed up as `gp32-dev/resume56-installed-core-before.so`.
RetroArch was absent during replacement, MainUI was available, and protected
settings retain their hashes.

## resume57: adjacent ALU operand loads not promoted

A bounded AArch64 candidate replaces two host loads with one W-form LDP
when an ordinary data-processing instruction reads adjacent guest register
slots. PC operands, immediate operands and register-controlled shifts retain
their existing paths; guest register/flag commits and all exit fences remain
unchanged. This differs from the resume10 LDM/STM transfer experiment.

H700 full native differential (9,510 events / 21 fallbacks) and stable-poll
equivalence pass. Qualified ABBA at observed 1,512 MHz preserves all seven
CPU/video/PCM fields, but produces only small mixed changes:

| Korea scene | Baseline core fps | Candidate core fps | Median change |
| --- | --- | --- | --- |
| Her Knights, warm 1200 / measured 1200 | 160.432 / 160.386 | 162.254 / 161.309 | +0.86% |
| Tomak, warm 300 / measured 600 | 112.933 / 112.780 | 113.181 / 114.286 | +0.78% |
| Little Wizard, warm 2100 / measured 300 | 402.845 / 406.325 | 401.685 / 402.794 | -0.58% |

The candidate was rejected: fewer emitted load instructions do not establish
a useful end-to-end improvement here. It was never installed. Source is
restored to resume56, and the rebuilt benchmark/core match the resume56
SHA-256 values exactly. No additional full-suite run is needed for those
identical restored artifacts. The candidate source/patch, binaries, qualified
results, test logs, `rejected.json` and `restored.json` are retained under
`F:/GP32/results/resume57-pair/`. Candidate benchmark SHA-256 is
`dfdf624b33a1bf339028cb16695b3debc996f865b1db69298d51ceda74962527`.

## resume58: preserve scheduler ticks with one task-table scan

The direct-FXE GPOS scheduler previously consumed the timer accumulator for
all expiries but processed at most 64 sleeper ticks. A large elapsed slice
therefore lost scheduler time. It also scanned the task table once per
processed tick. An elapsed counter at UINT32_MAX could wrap before its
deadline comparison and leave an expired task asleep.

The scheduler now passes the complete tick count to the sleeper update.
Each eligible sleeping task advances once using a widened sum, wakes when
its deadline is reached, and retains the existing wake state/reset behavior.
No guest task executes between the previously repeated updates. The SWI
sleep path explicitly passes one tick. This removes repeated table scans
and the 64-tick truncation without adding saved state or changing its format.

The regression contrasts one 100-tick batch with 100 one-tick updates,
checking a crossed deadline, a still-sleeping task and an already-expired
maximum counter. It fails on the old code and passes on the fix. Windows
and native H700 timer, PCM and state tests pass. Windows core and Android
ARM64/ARMv7 builds pass; Android runtime remains unverified.

Her Knights and Tomak H700 replays retain all seven CPU/video/PCM fields
from the resume56 baseline. These are exactness checks, not qualified speed
comparisons or fresh physical-speaker tests. The change concerns emulated
scheduler sleepers, not the guest callback dispatch limit. In particular,
the separate nested-callback CPU/peripheral time divergence remains open;
do not treat this fix as callback-time settlement or complete audio timing.

Evidence: `F:/GP32/results/resume58-time/` includes red/green test logs,
native test results, `game-equivalence.json`, build logs and deployment
record. Installed core SHA-256 is
`c089d6c238eaccfeea4062492f7554646ac6f67092199d9604f259a6e5c1530b`;
previous core backup is `gp32-dev/resume58-installed-core-before.so`.
Protected device settings remain unchanged.

## resume59: SDK PCM refill follows buffer boundaries

The SDK mixer previously checked the streaming refill callback once before
rendering the entire elapsed sample batch. Rendering could cross and wrap
both buffer halves without a refill. It also reset the 64-sample poll phase
instead of retaining the remainder. Thus identical elapsed time split into
different host slices could produce different PCM and stale buffer content.

Mix spans now stop at the recognized half-buffer edge or the next ordinary
poll boundary. A released half is refilled before its samples are reused;
the poll phase survives partial slices. Small halves and halves not aligned
to 64 samples are handled. Metadata is inspected again after a guest refill,
and a failed callback is retried at a boundary rather than on every short
host slice. Shift/range checks reject malformed half-buffer dimensions.
No runtime address cache or saved-state format change is introduced.

The regression runs a real ARM refill callback with JIT enabled and compares
one 10-ms batch against 100 smaller slices. Half sizes of 32, 64 and 70
samples all yield identical 441-frame stereo PCM, expected callback counts,
and the newly refilled samples on reuse. The original 64-sample fixture fails
on the old code. Windows and native H700 PCM, timer and state checks pass;
Windows and Android ARM64/ARMv7 core builds pass.

Her Knights and Tomak H700 replays retain exact CPU/video/PCM fields from
the prior baseline. They do not establish whole-game or physical-speaker
acceptance of the SDK streaming path. This is a refill-order correction,
not a measured FPS gain. Nested callback cycles still need proper peripheral
time settlement; that separate defect remains open.

Evidence: `F:/GP32/results/resume59-refill/` contains regression/build logs,
native test results, game equivalence and guarded deployment records.
Installed core SHA-256 is
`aea83fb1a649550162c670184543ac93ed13f3c7546a0ff4082cae4ccaf85c2e`;
the previous core is backed up as `gp32-dev/resume59-installed-core-before.so`.
Protected settings remain unchanged.

## resume60: remove unused SDK timer discovery and accelerate refill search

The audio polling path repeatedly searched for an SDK timer table, but its
only result was a cached address: timer dispatch had already moved to GPOS,
and the discovered table was never used at runtime. When no table existed,
every poll repeated the full search. Remove that unused discovery and keep
the legacy field in capture/load structures for v0002 state compatibility.
The 64-sample phase still bounds refill retries; timer callbacks remain owned
by GPOS rather than being re-entered from the audio mixer.

The separate, required PCM refill-descriptor search resolves its read-only
RAM window once per call and reads words directly. Windows crossing a RAM
boundary retain the guarded reader. Candidate validation and callback order
are unchanged, and the pointer is never reused after a guest callback or
across calls/state loads.

A native H700 synthetic SDK stream renders 2,205,000 stereo frames through
70-sample halves with real ARM refill callbacks. This fixture deliberately
has no legacy SDK timer table, exercising the former repeated miss path.
At observed 1,512 MHz:

| Variant | Measured seconds | Comparison scope |
| --- | --- | --- |
| resume59 baseline | 52.805916 / 52.684028 | Baseline sides of the initial ABBA |
| Direct RAM search only | 45.181192 / 45.124807 | Same ABBA, 1.168x throughput |
| Final change, including unused-discovery removal | 3.593649 / 3.594515 / 3.592362 / 3.592651 | Subsequent steady runs, 14.679x versus baseline median |

All rows retain audio hash `6ceffc2ff53a9aa1`, 2,205,000 frames, callback
counter and 189,000 callback CPU cycles. The final comparison reuses the
completed baseline measurements; it is not a new interleaved ABBA. Four
steady candidate records were collected despite a collector postcheck that
expected two; all four complete records were analyzed without rerunning.
Clock-ramping runs are excluded. This is synthetic mixer throughput, not
game FPS, physical playback or a typical-game improvement claim.

Windows/H700 PCM, timer and state checks pass; Windows and Android ARM64/
ARMv7 builds pass. Her Knights/Tomak replay outputs remain exact. Evidence:
`F:/GP32/results/resume60-scan/` contains the fixture, per-run clock samples,
`comparison.json`, `steady-comparison.json`, native checks and deployment.
Installed core SHA-256 is
`ec04a1bfea14ab8d46a443223b21c6adc01416a3b1aaaa67b16c7137796e0ab5`;
backup is `gp32-dev/resume60-installed-core-before.so`. Protected settings
are unchanged. Nested-callback peripheral time settlement remains open.

## resume61: HLE PCM clock-domain correction

HLE PCM now converts elapsed execution-budget cycles using the effective
run clock, matching `gp32_run_cycles` and the frontend's frame budget. It
previously divided those cycles by the firmware-visible PLL frequency.
With FCLK 66 MHz and run clock 33 MHz, a 10 ms slice consequently generated
only 220 stereo frames instead of 441 at 44.1 kHz.

The regression exercises the actual idle/peripheral/audio tick path with
mixed SEF and looping PCM input. FCLK/run ratios of 66/66, 66/33 and 132/48
MHz now produce 441 byte-identical stereo frames per 10 ms. The old core
fails the divided-clock case. Windows and native H700 PCM, timer and state
checks pass; Android ARM64/ARMv7 builds pass. Her Knights/Tomak replays
retain all seven CPU/video/PCM fields from the accepted baseline. These
replays do not establish audible acceptance of the corrected HLE path.

No PLL, clock heuristic, default option or saved-state layout changed; no
forced 166/200 MHz override was added. Nested-callback peripheral time and
elapsed-millisecond continuity across guest clock changes remain separate
open issues. This is an audio timing correction, not a measured FPS gain.

Evidence: `F:/GP32/results/resume61-pcm-clock/`, including the old-code
failure, native checks, build logs, replay comparison and installation record.
Installed core SHA-256 is
`c14b01fc0ef443966289cc1b686768c861ada27bfa2cfc144455a5a26bf61a76`;
backup is `gp32-dev/resume61-installed-core-before.so`. Protected settings
remain unchanged.

## resume62: reduce SDK mixer RAM-access overhead

Resolve the four-channel descriptor table once per callback-free mix span,
then read its live little-endian words directly. The table is not copied:
cursor writes and possible aliases still become visible to subsequent reads.
Resolve it again after a refill callback, and retain the guarded per-word
path when the table crosses RAM boundaries. Sample conversion also uses one
complete-range check for a 16-bit RAM read, keeping bytewise zero-fill for
partial samples at either RAM edge. Callback cadence, cursor writes and
saved-state layout remain unchanged.

At observed 1,512 MHz, an interleaved H700 ABBA comparison of the existing
SDK streaming fixture gives:

| Variant | Elapsed seconds for 2,205,000 stereo frames |
| --- | --- |
| resume61 baseline | 3.596697 / 3.594255 |
| candidate | 2.115468 / 2.128300 |

Median throughput is 1.694x baseline, about 41% less processing time. All
runs retain PCM hash `6ceffc2ff53a9aa1`, callback counter 2147547916 and
189,000 callback CPU cycles. The initial clock-ramping comparison is excluded;
the qualified comparison starts only after a whole warmup run observes
1,512 MHz. This is SDK mixer fixture performance, not whole-game FPS or
physical speaker acceptance.

A four-slot test checks mixed signed output, looping and one-shot sources,
invalid-source cleanup, cursor state, unaligned data, a partial descriptor
table and zero-filled edge reads. Windows/H700 PCM, timer and state checks
pass; Android ARM64/ARMv7 builds pass. Her Knights/Tomak retain all seven
accepted replay fields. The CPU backend is unchanged.

Evidence: `F:/GP32/results/resume62-mixer/`, including both frequency-sampled
comparisons, native checks, build logs and game equivalence. Clock-change
continuity and nested-callback time accounting remain open. In particular,
the CPU commits `cycles_total` only when `arm920t_run` returns: reading that
counter in a clock-register write handler alone cannot timestamp the write
within the active slice. A future timing fix must address that boundary,
as well as preserving old-state loading, before exposing clock overrides.

Installed core SHA-256 is
`b841afd42adabadfc8c30bd07e3ecfba0b5d868461a954a1f1dd132744f2dd07`;
backup is `gp32-dev/resume62-installed-core-before.so`. Protected settings
remain unchanged; `installed.json` records the verified deployment hashes.

## resume63: continuous firmware elapsed time across guest clock changes

Firmware milliseconds previously divided all historical CPU cycles by the
current effective clock. Lowering the clock could double already elapsed
time; raising it could move time backwards. Accumulate completed CPU/idle
slices at their own rates instead, carrying the fractional nanosecond phase.
Guest clock writes which change the effective execution rate end the current
CPU run after that instruction, so subsequent instructions are accounted at
the new rate. Host setup writes do not halt the next run.

The same accounting wraps synchronous HLE guest callbacks. Their existing
maximum execution budget is now charged in actual cycles, rather than a
fixed number of calls to `arm920t_run`; otherwise frequent clock-write yields
could exhaust the callback limit early. Reset and direct-image loading clear
firmware elapsed time independently of the cumulative CPU diagnostic count.

State format v0003 adds a 16-byte elapsed-time section while retaining the
v0002 component layouts and a v0002 reader. Old images have no clock history;
migration starts from their former observable time and integrates future
execution normally. New images preserve clock history and fractional phase.
Old cores cannot read v0003 images. The loader validates the new section
before committing machine state, retaining rejection without mutation.

The pre-change elapsed-time implementation fails the new regression. JIT-on
and JIT-off cases cover a guest 66-to-33-to-66 MHz transition, time across
save/load, reset, a clock-changing guest callback and 300 successive clock
yields. Windows and native H700 timer/PCM/state tests pass. The native H700
JIT differential suite (9,510 events, 21 fallbacks), stable-poll, SoC timing,
libretro audio and persistence checks pass. Android ARM64/ARMv7 builds pass.
The unchanged CPU/SoC results were reused after the callback-budget-only
followup; affected timer/PCM cases and the final H700 core replays were checked.

Her Knights and Tomak load their existing v0002 states and retain all seven
CPU/video/PCM replay fields. No whole-game FPS or speaker improvement is
claimed. This fixes the firmware elapsed-time counter, not complete peripheral
chronology: peripherals still use the existing post-slice update model, and
nested callbacks still require pending-event/budget settlement. Forced guest
clock overrides remain deferred.

Evidence: `F:/GP32/results/resume63-clock/`, including the old-code failure,
native checks, transactional state cases, final builds and old-state replays.
Installed core SHA-256 is
`df361f96b5ce4748ec1090d3a120161554464916293caf7b479a5f5b222a9bab`;
backup is `gp32-dev/resume63-installed-core-before.so`. Protected settings
remain unchanged. Existing user save files were not rewritten during deployment.

## resume64: retain HLE timer/audio phase across clock-changing callbacks

HLE consumers now receive the effective clock captured before their elapsed
CPU/idle slice. Previously a timer callback could lower the clock before the
audio mixers processed that same slice, making them generate extra samples
for time that had already elapsed. At the slice boundary, normalize the
timer and output-sample fractional accumulators to the final current clock.
Disabled sources retain their fractional progress too. Source resampling
and SDK refill polling phases use different units and are left unchanged.

The regression uses real guest timer callbacks to change 66-to-33-to-66 MHz,
with a separate 1 kHz counter and a state roundtrip after the first change.
The old PCM path emits 43 frames where only 21 are due. Both corrected HLE
PCM and SDK mixer paths emit 21, 22 and 45 frames over the three intervals,
while the counter stays at zero through the first two and reaches one in
the third. Fractional sample/timer progress survives the roundtrip.

Windows and H700 PCM, timer and state checks pass; Android ARM64/ARMv7 builds
pass. Her Knights/Tomak replay fields remain exact. CPU/SoC backends and the
v0003 save format are unchanged. This addresses elapsed-slice clock selection
and HLE fractional phase, not audible acceptance or game FPS. Hardware SoC
post-slice ordering, audio-source activation within callbacks and accounting
for nested callback time still require the separate event-settlement work.

Evidence: `F:/GP32/results/resume64-hle-phase/`, including the old-code
failure, native regression results, platform build logs and game comparisons.
Installed core SHA-256 is
`581a63b59d37f3f162cd9eb3e5e574e2a6fa0685ba0020bf496d4685999b716b`;
backup is `gp32-dev/resume64-installed-core-before.so`. Protected settings
remain unchanged, and user save files were not rewritten by deployment.

## resume65: preserve hardware PWM/IIS progress across clock writes

Clock/power register writes now capture the old and new PWM/IIS periods and
rescale their accumulated CPU-cycle progress. Whole pending periods and the
fractional position within a period survive a divider change, rather than
being reinterpreted under the new period. Existing run-clock-change yields
remain conditional on CPU execution. No CPU backend or state-format change
is involved.

New regressions fail against the previous library. All five PWM channels now
retain counter position and expire at the expected boundary after both divider
directions. IIS sample delivery retains its boundary across the same changes
and a state roundtrip. The existing LCD trace remains `47779e5037cd7b27`.
Windows and native H700 PWM, SoC timing, PCM, timer and state checks pass;
Android ARM64/ARMv7 builds pass. Her Knights and Tomak retain all seven replay
fields exactly. These checks establish phase preservation, not an FPS gain
or physical speaker acceptance.

Elapsed CPU-slice ordering at MMIO writes, LCD phase, PWM/IIS prescaler writes,
queued IIS sample-rate metadata and nested-callback event settlement remain
separate work. Evidence is in `F:/GP32/results/resume65-soc-phase/`.
Installed core SHA-256 is
`89062eb4582d845e93566cf261f5cb5e5ec9d91749ffbcd751078ff0a62f49ee`;
backup is `gp32-dev/resume65-installed-core-before.so`. Protected settings
remain unchanged; deployment did not rewrite user saves.

## resume66: settle hardware time before applying a guest clock change

The SoC now owns each CPU-run boundary. A guest clock write remains visible
to reads within that instruction, but peripheral phase conversion waits until
the completed CPU slice has advanced hardware at its original clock. Changes
to PCLK alone also yield, even when the effective CPU rate is unchanged.
Host setup writes still apply immediately. The runtime transaction is cleared
before returning and adds no saved-state fields.

Both main execution and synchronous guest callbacks use this boundary. Thus
callback instructions now advance hardware timers, DMA/IIS and LCD time, and
the main loop no longer ticks those cycles a second time. HLE timer/audio
event settlement and charging callback time against the outer host budget
remain unfinished; this change does not claim to resolve that larger design.

Real guest instruction regressions reproduce wrong PWM counters/expiry and
IIS delivery against the previous core. With PCLK halved while CPU speed stays
48 MHz, the corrected PWM IRQ lands at cycle 300 and IIS output at cycle 506,
with neither event one cycle early. A callback spanning a timer period now
raises its hardware IRQ. Interpreter and JIT paths pass on Windows and H700.

Windows PWM, timing, PCM, timer and state checks pass. Native H700 additionally
passes libretro audio and persistence checks (seven checks total). Android
ARM64/ARMv7 builds pass. Her Knights/Tomak preserve all seven CPU/video/PCM
replay fields. The unchanged LCD trace is `47779e5037cd7b27`; this does not
resolve LCD phase rescaling, prescaler transitions or queued IIS rate changes.
No whole-game FPS or physical audio acceptance claim is made.

Evidence: `F:/GP32/results/resume66-soc-boundary/`, including the old-code
failure, focused native checks, platform builds and exact game comparisons.
Installed core SHA-256 is
`37909b5a3946c5d696493771acef54767994840b5dff83223e9b7ada92d609f5`;
backup is `gp32-dev/resume66-installed-core-before.so`. Protected settings
remain unchanged; deployment did not rewrite user saves.

## resume67: retain LCD frame phase through clock changes

Clock application now converts the current LCD frame's progress to the new
CPU-cycle period. Completed frame history is removed before multiplication;
completed scanout counts are already stored separately. Previously, changing
48-to-24 MHz half-way through a frame reinterpreted the old cycle counter as
a whole new frame and jumped the scan position back to its start. The same
correction applies after the guest-write execution boundary from resume66.
The saved-state layout remains unchanged, including reading older accumulated
LCD histories. The existing approximate 60 Hz panel model is not replaced.

The old-code regression fails. A 5,000.5-frame history now retains scan position
through both divider directions, produces no extra frame on the write, and
emits the next scanout exactly after the remaining half-frame. Save/load retains
that boundary. Windows/H700 LCD timing, PWM and state checks pass; Android
ARM64/ARMv7 builds pass.

The old golden LCD trace `47779e5037cd7b27` included the phase jump and is
intentionally replaced with `db468039216226ad`. A separate rational
frame-fraction calculation reproduces both the legacy trace and the corrected
trace, including cycle quantization and save/restore, rather than accepting
an unexplained new hash. Derivation and observations are in
`F:/GP32/results/resume67-lcd-phase/derive_trace.py` and `.json`.

Her Knights/Tomak saved-scene replays remain exact in all seven CPU/video/PCM
fields. A fresh Windows BIOS-to-Her-Knights run reaches the same Korean title
capture as resume28; its CPU endpoint differs, so fresh-boot CPU equivalence
is not claimed. The first unqualified H700 replay was slower, requiring a
clock-observed comparison before drawing any performance conclusion.

The resulting Her Knights ABBA comparison is clock-qualified at 1,512 MHz
with all seven fields exact. Baseline throughput is 160.640/161.470 benchmark
frames/s; candidate is 161.405/160.620. Medians are 161.055 and 161.0125
(ratio 0.9997), effectively parity. Both versions also show a slower initial
warmup around 144 frames/s, so the first candidate replay alone was not
evidence of a regression. Governor settings were not changed. This throughput
is for the bounded replay, not the game's internal animation frame rate.

Evidence: `F:/GP32/results/resume67-lcd-phase/`. This is a frame-timing fix,
not complete-game, physical scanout or speaker acceptance. HLE callback event
settlement/budgeting, prescaler transitions and mixed-rate IIS queues remain
separate work.
Installed core SHA-256 is
`de1f9a1a42358766800249cba535c1350d05db1980b9bfe5da3e0918697734a4`;
backup is `gp32-dev/resume67-installed-core-before.so`. Protected settings
remain unchanged; deployment did not rewrite user saves.

## resume68: preserve queued PCM rate and match IIS DMA word ordering

IIS configuration writes and timing-cache refreshes no longer overwrite the
host PCM queue's rate without producing a sample. The rate is recorded only
when a complete stereo frame is appended, including the bulk DMA path.
Software-triggered DMA and direct FIFO writes refresh their IIS clock cache
before appending. This prevents an existing 11,025 Hz block being retagged
46,875 Hz solely because IIS was configured or ticked with no DMA source.

The 32-bit IIS DMA fast path also now pushes the upper halfword first, matching
the generic bus-write path. Previously the fast path reversed the word's
two samples, including when an earlier halfword awaited its partner. A
differential regression compares ordinary DMA channel 0 with fast channel 2,
both with and without that pending halfword, including the next FIFO write.
This establishes agreement with the existing MMIO model, not new hardware
measurement evidence.

Both defects reproduce against the previous core. Windows and H700 PCM,
SoC timing, state and libretro audio checks pass. Android ARM64/ARMv7 builds
pass. Her Knights/Tomak retain all seven CPU/video/PCM replay fields. The
changes are shared by desktop and libretro frontends; no state-format change
is needed. No speed gain or physical speaker improvement is claimed.

Evidence: `F:/GP32/results/resume68-iis-pcm/`. Actually appending blocks at
different rates before draining the queue still requires a segmented-rate
design; this change fixes metadata changes without new samples. Callback
event/budget settlement and prescaler phase changes also remain open.
Installed core SHA-256 is
`7c4efb6f92702f603f35568676ae3356a37c4d3a4d6bd925ad0ae2ca9c25a5da`;
backup is `gp32-dev/resume68-installed-core-before.so`. Protected settings
remain unchanged; deployment did not rewrite user saves.

## resume75: retry call folding after a cold callee mapping becomes available

Astonishia Story R's title music reproduces the reported slowdown without
a gameplay save. The BIOS auto-start reaches the title at frame 2400. A
300-frame continuation generates 115,677 stereo PCM frames. The earlier
silent boot-only samples do not characterize this workload.

Against `f710557`, the hot timer wrapper and its caller each dispatch about
19.25 million times in that window. Speculative call collection refuses a
missing/mismatched TLB entry, correctly avoiding page walks and device reads.
However, that cold failure permanently leaves the call split even after
ordinary execution establishes the mapping. Firmware and game code can
collide in the same TLB slot.

Blocks now remember a deferred call target. At the next dispatch where a
side-effect-free code peek succeeds, the ordinary translator retries call
collection. Unsupported callees retain their normal call. Existing fetch
revalidation, loaded-return checks, interrupt fences and stable-poll proofs
still apply. This is shared CPU code, with no game addresses or timing hacks.
The block field is derived JIT metadata, not serialized state.

On the Windows profiling build, the same title continuation reduces native
block calls from 62,329,492 to 6,594,886. Stable-poll skipped instructions rise
from 72,104,106 to 257,742,726 while the final CPU state, clock, generated PCM
count, video hash and PCM hash stay identical. These are logical work counts,
not an across-backend host-speed ratio.

H700 release builds were compared in ABBA order, with 120 warmup and 300
measured frames from the same title state:

| Run | Core frames/s | CPU frequency before / after (MHz) |
| --- | ---: | ---: |
| Before A1 | 35.651 | 480 / 1200 |
| After B1 | 183.644 | 1200 / 1416 |
| After B2 | 202.053 | 1416 / 1512 |
| Before A2 | 54.197 | 1512 / 1512 |

All seven output fields match across the four runs. Frequency ramping
disqualifies an exact aggregate speedup ratio; the governor was not changed.
The improved title window has substantial core headroom. This does not
establish combat minima, frontend frame pacing or physical speaker quality.

A synthetic cold-MMU-callee regression checks ragged budgets, register/RAM
equivalence against instruction execution, reduced dispatches, and modified
callee revalidation after guest I-cache maintenance. Its performance assertion
fails on the old core. Windows JIT, poll, exception and code-arena recycling
tests pass; H700 cold-callee and stable-poll tests pass. Short Windows Blue
Angelo NPC and Astonishia inn replays retain all seven output fields.
Android ARM64 and ARMv7 builds also pass; Android runtime is untested.

Private evidence: `F:/GP32/results/resume75-astonishia/`, including
`hot-profile.json`, `after-profile.json`, `h700-abba.json`, CPU test logs,
`regression-before.log` and `pc-game-equivalence.json`. ROMs and save states
are not committed.

The installed H700 core SHA-256 is
`98840170c667287b3ebd3f55b37c2d6f087ee436b52087bcb1aba78b1e17b161`.
The previous core is backed up at `gp32-dev/resume75-installed-core-before.so`.
Deployment verified the stock frontend, launcher and settings hashes unchanged;
it did not modify ROMs or saves. Reopen the game to load the updated core.

## resume76: copy contiguous IIS PCM and retain batching across DMA reloads

The active Astonishia title DMA descriptor (`0x50900060`) uses 16-bit units
and a 96-unit repeating source buffer. Its PCM was still appended one unit
at a time even when the whole source was known to be contiguous RAM.
For little-endian hosts, complete pairs can now use `memcpy`; an odd final
halfword remains in the FIFO. Fixed sources, a pre-existing FIFO halfword,
RAM-boundary spans and other transfer widths keep their existing path.

An IIS tick that crossed the DMA transfer-count boundary previously batched
only its first source span, then issued every remaining request separately.
It now rechecks the ordinary fast-path conditions after auto-reload and can
batch the next span as well. IRQ requests, stop/reload state and whole-service
request accounting are retained. Unsupported transfers keep the prior fallback.
There is no new state format, instruction-timing or sample-rate change.

In a DMA-only H700 workload of two million 32-frame ticks, baseline elapsed
times were 10.878546 and 6.083164 seconds; candidate times were 0.910186 and
0.910585 seconds. This synthetic workload omits guest CPU execution. Frequency
ramped from 480 to 1104 MHz in the first baseline and from 1104 to 1320 MHz in
the last baseline; both candidate runs started and ended at 1104 MHz. No exact
matched-clock ratio or whole-game FPS gain is claimed. PCM count/rate/hash and
the final DMA source pointer agree across all four runs.

Ordinary channel-0 FIFO writes provide an independent reference for the
16-bit copy path: odd/even counts, unaligned RAM, fixed addresses, pending
halfwords and the end of RAM are covered. Whole-tick versus single-period
execution agrees across odd and ordinary reload sizes, single/whole service,
auto-reload/stop, fractional-period carry, FIFO pairing and IRQ/DMA registers.
Windows and H700 PCM checks pass. Windows libretro audio, SoC timing, PWM and
state checks pass; Android ARM64/ARMv7 builds pass. Short Blue NPC and
Astonishia title replays preserve all seven CPU/video/PCM fields; the final
H700 title replay also matches the pre-change Windows reference.

Evidence: `F:/GP32/results/resume76-dma-pcm/`. The final review retained the
previous per-unit fallback after an unsupported transfer, avoiding a repeated
fast-gate check there; final PCM/device checks cover that adjustment. The
microbenchmark's supported RAM path is unchanged by that adjustment.

The separate IIS prescaler phase question remains open. The Samsung manual
defines the register fields, but does not establish live-divider-write phase
behavior; this optimization does not guess new hardware timing semantics.

Installed H700 core SHA-256:
`3f9ebb779eb3e063f042fa5a30a719d995a8466a502189935162fd4621188239`.
Backup: `gp32-dev/resume76-installed-core-before.so`. Stock RetroArch, its
launcher and protected configuration hashes are unchanged. Physical speaker
quality and whole-game performance remain separate acceptance work.


## resume77: run host frames by emulated time across clock changes

The former frontend budget sampled the CPU clock once per frame. A guest
STR changing the divider from 66 to 33 MHz made that frame last 33,333,318 ns;
the reverse transition lasted 8,333,348 ns. The hardware slice already yielded
at the register write, but the outer frame still consumed its original number
of CPU cycles. This was a shared pacing error, not a game-specific bottleneck.

`gp32_run_frame` now schedules a 60 Hz emulated-time deadline and recalculates
its remaining cycle budget after each CPU/idle slice. Synchronous guest HLE
callback execution counts toward the same deadline. Instruction/callback
overshoot carries into subsequent frames instead of adding another full
interval. The same clock-change probes now measure 16,666,681 and 16,666,666 ns.
This fixes time accounting; it does not raise the emulated CPU clock or promise
a throughput multiplier. HLE event settlement during callbacks and the
cycle-based vblank-wait bookkeeping remain separate follow-up work.

Libretro, Win64, Qt, SDL 1.2/3, WASM and automatic headless frames use this
common API. Explicit headless/SDL cycle budgets remain raw cycle execution.
Benchmarks now use the frontend pacing by default, record `frame_pacing`, and
provide `--legacy-cycle-frames` for comparisons with earlier measurements.
The raw-cycle Astonishia title replay still matches all seven prior output
fields (300 frames, 2,013,722,500 total cycles, 115,677 PCM frames).

State v5 adds a 16-byte deadline/fraction extension so rewind/run-ahead and
ordinary state loading retain frame cadence, including callback overshoot.
The loader still accepts v2/v3/v4 and rebases their frame pacing at loaded time.
New states require the updated core. Reset and explicit cycle stepping rebase
pacing. Truncated/invalid metadata is rejected before applying machine state.

Focused Windows checks pass: timer/frame pacing (interpreter and JIT), state
transactionality/migration, libretro audio, persistence and input. Sixty
frames stay within one instruction of one second; state restoration reproduces
the continuation and a real guest callback's frame overshoot. H700 timer/state
checks pass. Android ARM64 and ARMv7 libraries build. SDL1/SDL3/Win64 and the
WASM bridge compile as translation units; Qt and browser runtime acceptance
were not exercised in this change.

On H700 at observed 1512 MHz start/end, 180-frame native replays measured
212.013 core fps for the existing Astonishia title state and 81.926 for the
Blue Angelo left-to-NPC state. CPU state, clock, PCM count and video/audio
hashes match the corresponding Windows runs. These short core measurements
exclude stock RetroArch presentation and physical speaker/input acceptance;
they establish neither all-game performance nor a before/after speedup.
A fresh 2400-frame Astonishia boot plus 180 measured frames also ran on PC.

Private evidence: `F:/GP32/results/resume77-frame-clock/` (`before.txt`,
`after.txt`, `pc-games.json`, `device.json`, build logs and regression output).
The attempted CommandCode worker returned HTTP 429 without changes; all edits
were completed locally.

Installed H700 core SHA-256:
`4c71e7c4b0f1f21f93b772c3d724dc8208343d779fbcfed16fab429cdf0ed7cf`.
Backup: `gp32-dev/resume77-installed-core-before.so`. Stock RetroArch,
launcher and protected configuration hashes are unchanged. User ROMs and
saves were not modified. Reopen the game to load the updated core.


## resume78: inline modeled data-cache maintenance in both native JITs

The remaining H700 title profile included 39,501 coprocessor helper calls in
180 frames. The guest's cache-maintenance loop repeatedly executes an MCR to
c7/c14. In the current core, data-cache maintenance and write-buffer drain
(c7, CRm 6/10/14) only update the modeled c7 value; they neither invalidate
translated instructions nor change mappings. The decoder now retains those
operations in the trace, and x86-64/AArch64 emit the register store directly.
AArch64 can retain eligible maintenance loops inside a native block. PC
operands, I-cache maintenance, MMU/TLB changes and other coprocessor writes
keep their existing helper/exit behavior. Guest instruction counts, CP15 state
and interrupt checks are preserved; no hardware timing model is changed.

A differential fixture observes c7, executes successful/failed conditional
maintenance and compares ragged execution budgets against the interpreter.
The baseline fails its native-helper assertion and the candidate passes.
The focused maintenance plus modified-code cache checks pass on Windows and
H700. Windows CPU JIT, polling and exception tests pass; Android ARM64/ARMv7
libraries build. Short PC title/Blue NPC replays preserve all seven CPU/video/
PCM fields, and H700 title output matches the same pre-change reference.

For the same 180-frame H700 title, coprocessor helpers fall from 39,501 to 77
and native block calls from 3,459,717 to 3,390,725. Remaining helpers include
the necessary I-cache invalidations. Release measurements were 70.626 fps
before (480 to 480 MHz) and 77.109 after (480 to 720 MHz); frequency differed,
so these establish no matched-clock FPS speedup. Profile-instrumented FPS is
not representative of the release core. Evidence is in the private
`F:/GP32/results/resume78-jit/` directory.

CommandCode, SWE, OpenCode DeepSeek and 6.1 Sol attempts all ended with HTTP
429 before producing edits. The change was completed locally. The user asked
to finish only this in-flight optimization, then pause while fixing agent
availability; no subsequent optimization is started.

Installed H700 core SHA-256:
`44f999a512cd22f07d0a112b7cc5acc1950338d74c44d76e7cdf851ca859c815`.
Backup: `gp32-dev/resume78-installed-core-before.so`. Stock RetroArch,
launcher and protected configuration hashes remain unchanged.

### User-reported acceptance gaps at pause

- GP32 BIOS UI menu changes produce a tick/click before sound plays. PC Link
  itself is not a priority; the transition artifact remains unresolved.
- Astonishia Story R opening video sometimes produces crackling audio.
- Frame delivery is still not uniformly satisfactory in real use.

These are user observations, not newly reproduced or diagnosed cases. The
short title/NPC core replays above do not cover these audio transitions or
opening-video acceptance. On resumption, distinguish discontinuous source PCM,
resampler/rate transitions and frontend underruns before attributing crackle
to CPU throughput. Favor usable sound, frame delivery and input over speculative
cycle-perfect ARM920T pipeline/wait-state modeling. Greater timing fidelity
can fix scheduling errors but is not itself a host-performance optimization.
No new investigation was started after the user's pause request.

## resume79: audio boundaries, frontend lifecycle and paired A64 transfers

Development resumed with nine bounded agent assignments across CommandCode,
OpenCode DeepSeek, SWE and Sol. Sol handled the native A64 emitter; the parent
handled peripheral timing, integration and device validation. No title-specific
conditions or guest clock changes were introduced.

### Common peripheral audio fix

IIS/DMA register writes during a CPU batch previously changed the state used to
tick the entire elapsed batch. A stop at instruction 600 could discard a sample
due at cycle 500; splitting the same execution into single instructions kept it.
The SoC now stops after the writing instruction, ticks its elapsed prefix using
the previous peripheral state, then commits the stores in order. The transient
queue covers 16 STM stores including four-byte bus decomposition of each
unaligned word. Saved-state format is unchanged.

Regression coverage compares batched and instruction-stepped start/stop,
DMA-stop and divider writes, PCM/rate/count, plus aligned/unaligned full STM
against immediate bus writes. Windows timing/PWM/PCM/timer/state checks pass;
H700 timer and PCM checks also pass. The same timing fix applies to every
frontend. It fixes a reproduced sample-loss defect, but does not establish that
the user's BIOS click or opening-video crackle has disappeared.

### A64 transfer optimization and measured limits

The already-validated single-page RAM path now pairs LDM/STM word transfers.
Contiguous guest registers use paired context accesses too; sparse registers
retain individual context accesses. PC, cross-page and MMIO paths keep their
existing checks. The hot four-register transfer body falls from eight to five
host instructions. H700 differential tests cover register lists, partial
budgets, writeback, page boundaries, MMIO IRQ/invalidation, self-modifying code
and completion of an STM before a requested CPU yield.

A first 180-frame title comparison at 1512 MHz measured old HEAD 212.428 fps
versus combined changes 198.458 fps. Longer isolation was needed before drawing
a performance conclusion. In a 600-frame sequence, the SoC fix with the previous
JIT measured 194.322 and 193.265 fps; the paired JIT measured 195.074 fps, all
starting/ending at 1512 MHz. The old HEAD run measured 195.506 fps while ramping
1320 to 1512 MHz. Other short runs also changed frequency. These results show
only a small paired-JIT benefit in this workload, not a large overall speedup
or a reliable old-HEAD speedup. No governor setting was changed.

Title and Blue Angelo NPC H700 replays match the current PC core on all seven
CPU/video/PCM fields. The title's PCM and video match the old version as well;
its final PC differs because register writes now end the CPU batch. These are
core throughput measurements, not stock RetroArch presentation or all-game
frame-rate guarantees. PC startup/intro captures reached the Sonnori logo and
the opening text; speaker listening remains unverified.

### Frontend fixes

- WASAPI now submits only real queued frames. A short producer ring while the
  endpoint still has buffered audio previously inserted a fade/silence hole
  and raised a spurious underrun. A fake-endpoint regression fails before the
  patch and passes after it. Existing prebuffer settings stay unchanged.
- SDL3 reopens its audio backend with the same options after a successful
  state load, discarding sound queued before the load. Failed loads and
  `--no-audio` keep their existing behavior. This uses the existing lifecycle
  pattern; the normal startup buffering delay still applies. C23 compilation
  with the actual SDL3 headers succeeds; real device listening is unverified.
- Web Audio status messages carry the reset generation. The main thread
  rejects pre-reset snapshots that previously resurrected stale queue depth.
  The caller supplies the generation explicitly, including resets before node
  creation. A harness using the actual initialization/reset/message handlers
  verifies early and live reset cases; JS syntax checks pass. Browser playback
  is not newly verified.

The libretro delivery audit found no unintended loss/reordering under healthy
or partial consumption; forced overflow followed its documented bounded-drop
policy. The resampler audit found no deviation from its current phase model.
SDL1 needed no demonstrated backend fix. Android ARM64/ARMv7 C23 builds pass;
the ARM64 ELF already uses 16 KiB LOAD alignment, so no extra build change was
needed. Android physical-device execution remains unverified.

### Remaining audio lead and cleanup

The libretro probe captured a large discontinuity already present in source
PCM: a low-amplitude tail ends in one `0x8000` sample. Delivery preserved it.
Whether the guest supplied that value, a DMA boundary emitted it, or another
source-side error generated it still needs tracing. Do not hide it with a
title check or an arbitrary fade. SDL1 probes also expose rate-transition
interpolation steps; matching the current resampler model is not an acoustic
quality guarantee. Physical BIOS/menu/opening sound acceptance remains open.

Removed five orphan debug-symbol files (8,708,096 bytes). An inventory wrongly
classified the active resume75 profiling build as disposable; it was retained.
The executor rejected recursive deletion of four completed-worker Zig cache
directories (106,499,291 bytes), so those remain. ROMs, BIOS, saves, worktrees,
active builds and diagnostic evidence were preserved.

Private evidence: `F:/GP32/results/resume79-source-audio/` and sibling
`resume79-jit`, `resume79-win-audio`, `resume79-sdl-audio`, `resume79-wasm-audio`,
`resume79-libretro`, `resume79-resampler`, `resume79-sdl12`, `resume79-android`
and `resume79-cleanup` directories.

Installed H700 core SHA-256:
`f8ade8cb42a845b5b85aa3518151eaf08aeafd211fcd1c2f2c3d97ed4703a44d`.
Backup: `gp32-dev/resume79-installed-core-before.so`. Stock RetroArch,
launcher and protected configuration hashes are unchanged. Reopen a game
to load this core; no settings adjustment is required.

## resume80: preserve audio tails and trace BIOS-origin discontinuities

WASAPI no longer stops/resets an endpoint merely because its remaining audio
is at most 256 frames. It waits for zero padding, preserving the real tail
(up to 5.3 ms at 48 kHz) before the existing rebuffer policy. The targeted
mock-endpoint test fails before and passes after the change. SDL3 now includes
the incoming batch when deciding whether to discard excessive backlog,
preserving soft-limit drop priority and the existing whole-batch acceptance
policy. Ordinary-stream PCM/counts are unchanged in its focused probe;
cap-sized-or-smaller burst submissions no longer leave an oversized queue.
A single batch larger than the cap can still exceed it, as in the other
backends. Settings and fade constants were not changed. Native speaker
acceptance remains unverified.

The resume79 source impulse was traced to an original zero byte in the old
Korea v1.5.6 diagnostic BIOS: its guest mixer converts that U8 value to
0x8000 and writes both channels before DMA reads them. JIT/interpreter boot
WAVs match. H700 actually loads the Europe v1.6.6 image from its ROM directory,
whose boot sound has no such terminal impulse. With that matching image,
scripted BIOS menu changes do produce large negative PCM tails, again from
guest mixing of original BIOS bytes and with identical JIT/interpreter output.
This narrows the source of the values but does not prove that physical DAC,
mute or analog output behavior is already emulated correctly. No game/BIOS
special case, asset modification or speculative fade was added.

Private evidence: `F:/GP32/results/resume80-source/README.md`,
`resume80-win-drain/` and `resume80-sdl-limit/`. The
[MAME GP32 driver](https://github.com/mamedev/mame/blob/master/src/mame/gamepark/gp32.cpp)
was consulted for its L3 pin handling and IIS output; it is not a verified
analog-output reference. Hardware/output-stage investigation remains open.

## resume81: Princess Maker cutscene workload and frontend follow-up

The user-provided Princess Maker 2 slot 0 was copied read-only and verified
against the remote SHA-256. Its 300-frame PC replay executes 101,079,269 ARM
instructions through portable decoded blocks and 67,866,349 through native
blocks. Of the portable work, 101,069,530 instructions belong to one eight-op
loop that decrements a counter on every backedge. The current polling-candidate
gate excludes it from native compilation even though its changing counter
prevents a fixed-point skip. The compiler now admits conservative straight-line
counter loops with one backedge and a nonzero unconditional self ADD/SUB whose
destination has no other possible writes. Early exits remain guarded. No game
address or instruction signature is part of the classifier.

In the same 300-frame replay, portable work falls to 21,677 instructions and
native work rises to 168,923,941. Existing poll-skip counts remain exactly 22
events / 554,382 instructions, and all seven CPU/video/PCM fields match.
A pinned-process PC ABBA gives 262.521/314.813 fps before and
370.973/403.964 after (mean +34.2%); the host was busy and these samples retain
timing variance. They do not establish H700 speedup. Focused x64 differential
tests pass for changing loops and false-progress/stable-poll cases. A53 static,
H700 libretro/bench, Win64 GUI and Android ARM64/ARMv7 C23 builds pass; native
A64 execution and device presentation remain pending.

The same saved cutscene advances through text during a 600-frame PC replay,
with PCM present in every frame and six image changes. These are static slide
and text changes, not a requirement for 600 distinct images. PC profiling does
not measure H700 presentation speed. Device benchmarks and core replacement
are deferred while the user plays other games. Sound work remains active:
the reported Dooly Soccer startup pop is being compared at source and frontend
delivery boundaries; original waveform values alone do not prove hardware
speaker behavior.

A bounded Dooly capture matches a queue-free replay of the existing resampler
and silence policy bit-exactly; no additional delivery-queue glitch was found.
The recorded resume edge of 0 to 5137 and large intra-span source steps still
need to be correlated with the audible report. This does not validate the
source emulation or output model, and does not isolate the cause to the driver.

Win64 now clears keyboard state on focus loss, preventing a key release sent
to another window from leaving GP32 buttons pressed. A hidden-window probe of
the actual window procedure passes press/focus-loss/fresh-press/release checks.
The Win64 GUI and SDL3 DLL build successfully in Release C23; a full GUI/audio
session has not been exercised. Indexed fallback LCD rendering now expands its
reachable palette entries once per scanout, preserving all 18 synthetic
fallback pixel hashes. Host timings are noisy and do not establish a real-game
speedup; this path is not the identified Princess Maker CPU bottleneck.

Private evidence: `F:/GP32/results/resume81-princess/`,
`resume81-indexed/REPORT.md`, `resume80-integration/focus-result.log`,
and `resume80-win-gui/README.md`. The prior A64 memory-helper candidate also
still requires native A64 runtime and performance acceptance before deployment.

### H700 acceptance after the user finished playing

The final combined A53 binary passes checked-access, changing-counter-loop
and stable-poll fixtures. Princess Maker, Astonishia title and Blue NPC replays
match all seven PC CPU/video/PCM fields. At 1512 MHz throughout each measured
run, interleaved ABBA averages are:

| Fixed scene | Before core fps | After core fps | Ratio |
| --- | ---: | ---: | ---: |
| Princess Maker slot-0 cutscene, 300 frames | 20.585 | 54.995 | 2.672x |
| Astonishia title, 180 frames | 198.474 | 199.536 | 1.005x |
| Blue Angelo NPC approach, 180 frames | 82.082 | 82.143 | 1.001x |

The last two changes are small enough to treat as approximately flat.
Princess remains below 60 core fps, so the performance goal is not reached.
The improved core is installed with SHA-256
`b89cdb612f6a10c1bc7d20989e5ba8334228c82ba0d07a9f7600d5fbc6f30dab`.
Previous core: `gp32-dev/resume81-installed-core-before.so`. Stock RetroArch,
launcher and protected configuration hashes are unchanged. Physical sound and
full RetroArch presentation acceptance are still open. Evidence:
`F:/GP32/results/resume81-princess/device.json` and `installed.json`.

The captured loop's two base registers address GPBDAT and GPEDAT. Repeated
button-bit assembly inside these real GPIO reads is the next measured-path
candidate; input sampling and SmartMedia side effects must stay unchanged.

### GPIO input derivation and idle PCM continuity (2026-10-04)

GPIO button bits are now derived when host input changes, on reset and after
state load, instead of being assembled on every GPBDAT/GPEDAT read. Reads still
compose the current SmartMedia signals; no MMIO reads or guest time are skipped.
The serialized machine layout is unchanged. Focused GPIO, input, state and SoC
timing checks pass. H700 native GPIO tests and the same three replay hashes pass.
At 1512 MHz, warmed ABBA Princess cutscene means are 55.049 -> 56.202 core fps
(1.0209x); Astonishia title 198.384 -> 198.804 and Blue NPC 82.386 -> 82.471
are approximately flat. This remains below the 60-fps target.

Libretro idle silence now passes through the guest-rate resampler, preserving
the carried endpoint and fractional duration across idle/active transitions.
An aligned 44.1-kHz copy retains its last sample and next-output timestamp, so
it remains an exact memcpy path while supporting a subsequent rate change.
No additional fade, volume adjustment or title-specific rule was introduced.
Analytic sample-grid tests cover both channels, blocked/partial callbacks,
rate changes, fractional counts and reset. Existing queue/lifecycle tests pass
on Windows and native H700; Win64 and Android ARM32/ARM64 builds also pass.

Replaying the previously captured Dooly source PCM through the actual changed
libretro delivery code reduces the measured stop/resume edge peaks from
3469/5137 to 880/1286 sample units, with no pending delivery backlog. This is
digital boundary evidence, not proof that the user's intermittent speaker pop
is fixed; the captured guest PCM's large internal discontinuities remain.

Private evidence: `results/resume82-princess/device.json`,
`results/resume82-gpio/SUMMARY.md`, and `results/resume83-audio-fix/` under the
local GP32 workspace.

After the user reconnected the device, this tested core was installed with
SHA-256 `edcf176fb68995f55281d6caa5e4d08add8aa1ad7099f78e0c8e6fc56126813a`.
The previous b89cdb61 core is preserved at
`gp32-dev/resume84-installed-core-before.so`. The promotion guard confirmed
RetroArch was stopped and MainUI was present. Stock frontend, launcher and
configuration hashes were unchanged. Evidence: `resume84-install/installed.json`.

The user's volume remains untouched at their selected zero setting. No game
frontend or mixer command was run during promotion. Continue digital PCM,
buffer and timing validation silently; audible speaker acceptance remains open.

### IIS transmit-FIFO restart (2026-10-04)

The newly collected [Mirko SDK 0.91](https://dl.openhandhelds.org/gp32/uploads/Home/GP32%20-%20Development/Libraries/mirkoSDK091.tar.gz)
uses IISFCON TX-disable as a FIFO flush in `lib.src/sound/init1330.c`,
`GpPcmStop` (lines 274-279). The [Samsung S3C2400 manual](https://datasheets.chipdb.org/Samsung/S3C2400.pdf)
identifies TX FIFO enable as IISFCON bit 9 (chapter 21). The related
[Linux Samsung I2S driver](https://code.googlesource.com/linux/torvalds/linux/+/c6e169bc146a76d5ccbf4d3825f705414352bd03/sound/soc/samsung/s3c24xx-i2s.c)
also disables the FIFO on stop to reset pending transfer state. No external
implementation code was copied.

The emulator previously retained a pending unpaired channel across TX-disable.
A public-API sequence with one completed frame and one pending channel then
restarted playback, incorrectly pairing the stale channel with the new stream.
The falling edge of bit 9 now clears only that pending FIFO state; completed
host PCM remains queued. Same-value enable writes and unrelated byte lanes
preserve the partial pair. This adds no serialized fields or title checks.

The focused restart case fails before the change and passes after it. Existing
SoC timing, PCM and state tests pass on Windows; the same timing/restart fixture
passes natively on H700 without audio playback. Win64 and H700 cores build.
Private evidence: `resume84-fifo/h700-test.txt`. Correlation with a particular
game's audible pop remains unverified; this is a reproduced FIFO lifecycle bug.

The FIFO fix is installed on H700 with SHA-256
`f46d2a1a6ce2ef543780214d68ed698d548ba63e3d5bb4da5e77ad9b6b3023d2`;
the preceding core is backed up as `gp32-dev/resume84-fifo-installed-core-before.so`.
Stock frontend/launcher/configuration hashes are unchanged. This promotion and
all subsequent device checks leave volume untouched and produce no audible
playback. Win64 and Android ARM32/ARM64 builds pass. Evidence:
`resume84-fifo/installed.json`; staged PC binaries: `resume84-fifo/pc/`.
