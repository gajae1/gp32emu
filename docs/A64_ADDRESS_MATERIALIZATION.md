# Shorter CPU-field address materialization

2026-10-05. The AArch64 emitter now uses ADD-immediate, including its shifted
12-bit form, for CPU-structure offsets below 16 MiB. Large field loads/stores
keep the low aligned part in their scaled memory operand. This replaces a
MOVZ/MOVK/ADD sequence with one or two address instructions; the old fallback
remains for larger offsets. Guest instruction order, registers and cycle
accounting are unchanged.

Worker evidence is under `F:/GP32/results/round134-performance/a64/`.
Hybrid dispatch-weighted instruction estimates decrease 1.32% in Her Knights
and 0.87% in Funny Soccer; these are not measured H700 FPS improvements.
111 completing fast-path blocks matched the interpreter in a private Unicorn
execution comparison. Helper, PC-relative and self-loop coverage there was
limited to structural/dispatch evidence.

Parent validation executed the actual emitted AArch64 code on H700:
`arm_jit_test` and `arm_jit_recycle_test` both pass, including flags, memory,
budgets, exceptions and arena lifecycle. Tests were staged in RAM; eight
protected files were unchanged. Evidence: `round133-parent/checks.json`.
This has not replaced the installed core; Android runtime remains unverified.

## Rejected parallel LCD candidate

A separate unchanged-row scanout cache was tried and removed from production.
All per-frame hashes matched across nine synthetic patterns, but the H700
16-bit static/one-row cases became approximately 2.67 times slower at the same
720 MHz (about 421 us to 1,123 us for static). Full 16-bit redraw also regressed
(420 us to 600 us). Modest indexed-color gains did not justify that regression.
PC-only scanout speedups are insufficient to promote this candidate on H700.

Its worker is refining it privately; raw native evidence is retained under
`F:/GP32/results/round136-lcd/`. No LCD-row cache is included in the accepted
source. Device volume, governor, menu integration and installed core remain
unchanged.
