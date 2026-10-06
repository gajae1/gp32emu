# Shorter CPU-field address materialization

2026-10-05. The AArch64 emitter now uses ADD-immediate, including its shifted
12-bit form, for CPU-structure offsets below 16 MiB. Large field loads/stores
keep the low aligned part in their scaled memory operand. This replaces a
MOVZ/MOVK/ADD sequence with one or two address instructions; the old fallback
remains for larger offsets. Guest instruction order, registers and cycle
accounting are unchanged.

Worker evidence is under `F:/GP32/results/round134-performance/a64/`.
Hybrid dispatch-weighted instruction estimates decrease 1.32% in Her Knights
and 0.87% in Funny Soccer; these are not measured handheld FPS improvements.
111 completing fast-path blocks matched the interpreter in a private Unicorn
execution comparison. Helper, PC-relative and self-loop coverage there was
limited to structural/dispatch evidence.

Parent validation executed the actual emitted AArch64 code on the handheld:
`arm_jit_test` and `arm_jit_recycle_test` both pass, including flags, memory,
budgets, exceptions and arena lifecycle. Tests were staged in RAM; eight
protected files were unchanged. Evidence: `round133-parent/checks.json`.
This has not replaced the installed core; Android runtime remains unverified.

## Rejected parallel LCD candidate

A separate unchanged-row scanout cache was tried and removed from production.
All per-frame hashes matched across nine synthetic patterns, but the handheld
16-bit static/one-row cases became approximately 2.67 times slower at the same
720 MHz (about 421 us to 1,123 us for static). Full 16-bit redraw also regressed
(420 us to 600 us). Modest indexed-color gains did not justify that regression.
PC-only scanout speedups are insufficient to promote this candidate on the handheld.

Raw native evidence is retained under `F:/GP32/results/round136-lcd/`. No
LCD-row cache is included in the accepted source. Device volume, governor,
menu integration and installed core remain unchanged.

### NEON row-comparison follow-up: also not promoted

The worker traced the original slowdown to executable-local Zig compiler-rt
`bcmp`, which compared equal spans byte by byte. Replacing that comparison
with bounded NEON loads/XOR/reduction restored isolated static-row gains:
16-bit scanout 639.7 to 366.6 us at 480 MHz, 8-bit 683.6 to 140.4 us at
720 MHz. Nine 240-step synthetic framebuffer hash streams still matched.
Full redraw/palette churn retained snapshot-copy overhead; 16-bit full redraw
was 423.8 to 608.3 us at 720 MHz.

Actual Princess Maker 2 slot0 ABBA replays did not justify promotion. After
excluding governor ramp-up samples, unchanged-frequency 1.512-GHz runs were
99.768 baseline versus 96.378 candidate frames/s. Moving the cache after all
existing SoC fields preserved their offsets, but did not resolve the
regression: 99.820 baseline versus 95.518 candidate. These bounded results
do not establish a universal regression magnitude; they reject this candidate
for the target workload. No further speculative variants were tried.

The before/after executables shared every core object except the SoC. Each
run used 600 warmup plus 300 measured virtual frames. Guest cycles, PC, CPSR,
clock, PCM count and video/PCM hashes matched. Protected device files matched.
Both refinements were removed from production; previous accepted test results
remain applicable. Private source/patches, native binaries and raw measurements:
`F:/GP32/results/round137-lcd-neon/`. The full-core slowdown's precise cause is
not established by this experiment; it must not be attributed to field layout.

## Rejected SoC bus-dispatch candidate

A worker candidate reordered fastmem to probe RAM before BIOS, routed MMIO
read8/read16/read32 through the switch dispatcher with one word read per
in-word halfword, and rewrote range checks in subtraction form. Its MMIO audit
found every read path pure or idempotent within one cycle, and all 24 PC tests
passed. PC microbenchmarks improved MMIO dispatch by 22-47%.

On the handheld at a stable 1.512 GHz (600 warmup, 600 measured frames, ABBA), guest
output matched in all scenes, but Princess Maker 2 slot0 fell from 99.6/99.3
to 97.1/96.4 frames/s (about 2.6%). Astonishia title (189.4 vs 189.2) and Blue
Angelo (77.3 vs 77.0) were unchanged. A control build differing only by an
empty asm statement measured 99.0/99.5 against 99.6/99.6, so code placement
alone did not reproduce the loss. The candidate was not promoted.
Evidence: `F:/GP32/results/round138/device-memory.json` and `layout/`.
