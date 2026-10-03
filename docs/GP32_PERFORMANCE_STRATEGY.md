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

The latest H700 core-only windows include Blue Angelo gameplay at 54.705 fps
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
independent experimental gains. The two current scenes still need about 1.10x
and 1.25x throughput respectively to clear 60 before RetroArch overhead.
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
