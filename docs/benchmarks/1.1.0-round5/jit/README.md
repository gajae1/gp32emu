# Round-five native-backend audit

The native mapping and code-write correctness fixes are default changes. Both
new x64 predicate optimization experiments are rejected. The final runtime
keeps the original `x64_emit_cond_skip`; the measured candidates remain as
exact patches and evidence, not default code.

## Correctness work

`36749228df8d4d8f3c517da13ea3703d4a1c5888` adds an independent 256-case native
predicate oracle covering all sixteen condition codes and all sixteen NZCV
combinations, EAX operand forwarding across conditional joins, and an observable
memory callback that changes a register and Z. It requires exact guest results,
callback order, retirement, and 1,792 positively counted native instructions.
The corresponding 512-case internal-forward matrix also passes.

The initial native direct-mapping correction is `479bd2b590c0c989859db066f8783bd2446b9c55`.
Its rejected endpoint proof is corrected by
`a456739d4236586c60567c15505f40600da1b284`. Apply both together. See
[mapping-proof.md](mapping-proof.md) for the contract, before failures, safe
complete-range certificate, x64/A64 results, and real SoC copied-bus state-load
controls. The original baseline already had the same endpoint problem in its
A64 open-bus constant path; this audit therefore fixes that pre-existing path
as well as generic native word I/O routing. The sibling `eae2f78` portable memory
fix owns all six cached accessors and the 2,478-case full operation matrix.

The native I/O store handovers also call the common CPU code-write notifier
after either a direct store or a callback store. That call occurs before
writeback and the existing generation/epoch continuation fence, after all
callback-sensitive decoded fields have been captured. The sibling rare-region
tracker `89184ed82f5982d075420e1b0b5a81be444d0c35` supplies the matching
common invalidation policy; exact combined
snapshots and final results are in `nonram-final-combined-*`. The earlier
`nonram-combined-*` failure is retained: its fixture incorrectly assumed cached
admission for a cold non-RAM PC with I-cache enabled. The corrected fixture
explicitly prepares the cached target, retains the same architectural oracle,
and additionally exercises public-dispatch BL-stitched extra-region leaves.

## Rejected performance experiments

The prior review reports and benchmark notebooks were checked before these
experiments. In particular, packed A64 TLBs, paired operand loads, framed/leaf
splicing, RAM mapping reuse, and the optional two-way A64 chain were already
studied; none is presented as new here. This experiment concerns x64 predicate
emission only.

The first candidate represents HI/LS with one comparison of the masked C/Z
bits and computes signed N/V comparisons with a shift/XOR/test. The second,
smaller candidate keeps only HI/LS; GE/LT/GT/LE remain exactly baseline. Both
preserve the forwarded value in EAX and retain the established branch patching,
helper handovers, block accounting and guest flag storage.

| Experiment | Target native result | Generated block bytes | Blocking control result | Decision |
| --- | --- | --- | --- | --- |
| Full compound predicates | Most changed cases improve about 2–8% median time | HI/LS 702→662; GE/LT 690→666; GT/LE 742→718 | GCC raw controls regress 5.26–7.89%; one Clang cached mixed control +4.36%; cold compile mixed | Rejected |
| HI/LS only | Target medians improve 1.19–4.50% GCC and 1.99–4.47% Clang | HI/LS 702→662; all other conditions unchanged | GCC raw controls regress 7.68–9.12%; paired medians +7.99–9.86% | Rejected |

These are synthetic-kernel results on this host, not whole-game or platform
speedups. The GCC raw-only regressions are decisive even though those runs
never execute the changed emitter. In the first variant, normalized raw
`exec_arm_at` assembly changes its register allocation and grows from 0x20a5
to 0x20ad bytes. The smaller variant restores the same handler address
(0xcd40), size and normalized instruction sequence, yet still regresses raw
execution. Handler identity alone does not establish that surrounding code
layout and execution costs are unaffected. No compiler-specific default gate
was added, and no further variants were timed.

[The complete result tables](predicate-results.md) contain every native,
cached, raw and compile scenario from both studies, including unchanged
controls, regressions and paired medians. JSON retains every individual sample,
range, binary SHA256, command, CPU affinity and UTC start/end timestamp.
The JSON is reformatted with one result per line for review; every parsed
measurement value is preserved.

### Workload and validation

`scripts/bench_native_predicates.c` executes a deterministic ten-instruction
loop containing a RAM load, CMP, four conditional arithmetic/logical
instructions, accumulation, loop-count subtraction and a backedge. A terminal
SWI checks its immediate and PC and stops the run. Expected results use an
independent C subtraction flag calculation and Boolean condition table. The
mixed input sequence is deterministic; always-skipped and always-executed
sequences are separate cases.

There are 38 scenarios per variant: 22 native scenarios (EQ, HI, LS, GE, LT,
GT and LE with all three outcome patterns, plus unconditional AL), eight
cached/raw controls (GT with three patterns plus AL), and eight cold-compilation
scenarios. Each timed run uses 32,768 elements and 64 passes after two warmup
passes; each cold sample compiles 1,000 blocks. Results are ns per element,
which represents ten guest instructions, and ns per compiled block respectively.
Compilation counters report arena growth including alignment; native
`code_bytes` records the single prepared block's emitted allocation.

GCC 13.3.0 and Clang 18.1.3 builds use C23 (`-std=c2x` and `-std=c23`
respectively), `-O3 -DNDEBUG -D_XOPEN_SOURCE=700`, no IPO/LTO, and profiling off
for timing. Separate profiling-on and profiling-off runs verify every scenario
with the clock explicitly disabled. The profiling runs each count 225,368
native instructions across the 44 before/after native verification rows; cold
compilation has zero guest execution. All four functional combinations pass for
each study. Fresh GCC and Clang builds of the smaller candidate also pass the
256-case all-NZCV/helper-join and 512-case internal-forward oracles.

The frozen baseline is `a456739d4236586c60567c15505f40600da1b284`.
`predicate-sources.json` and `predicate-hi-sources.json` identify the exact
sources, flags, driver hashes and candidate deltas. Neither benchmark variant
includes the later, separate rare-region native store-notifier call. That
workload performs only RAM data reads; the measured source difference is solely
the named condition-emitter patch.

### Timing method and limitations

The host reports an AMD EPYC 9V74 under KVM. Each subprocess inherits affinity
to permitted CPU 0. Root and both sibling agents explicitly paused builds,
tests and other timing throughout both windows. GCC and Clang ran sequentially;
seven neighboring before/after pairs were retained for every scenario. The
host is virtualized and frequency/scheduling were not independently controlled,
so individual outliers and control behavior remain visible instead of being
removed. QEMU was not used for these measurements.

**First-study ordering correction:** the original runner rotated scenario
order by the repeat number, which canceled its intended repeat/index parity.
Each individual scenario therefore kept one within-pair order across all seven
rounds, with both orders represented across different scenarios. It was not
an alternating-within-case study. Its exact runner is archived as
`predicate-first-runner.py`; the raw JSON samples are unchanged and each file
has an explicit method note.

The single follow-up uses the corrected current runner: stable original
scenario index determines before/after parity while scenario order rotates
independently. Every scenario's recorded pair order was checked to alternate
across all seven rounds. Both input patterns and both pair orders still show
the GCC raw regression. Cold compilation is around one microsecond per block
and has visible noise; the tables report it without converting it into a
whole-application claim. No rejected runtime candidate is enabled by a build
flag in the final product.

### Reproduction

Export the named baseline into an isolated directory and apply either
`predicate-candidate.patch` or `predicate-hi-only.patch` to a separate copy.
Keep the matching `arm920t.h`, `arm920t_jit_a64.inc` and iOS include adjacent
to each exported `arm920t.c`. Build the driver twice with `CORE_UNDER_TEST`
pointing to the respective exported source, using the compiler flags above
and the repository `src`/`include` include paths. Exact argument arrays are
retained in every `predicate-build-*.log`.

Run the current driver with `--verify` first. For the corrected measured
follow-up, run `scripts/bench_native_predicates.py` with `--before`, `--after`,
`--output`, `--rounds 7 --elements 32768 --passes 64 --compile-passes 1000` and
one permitted `--cpu`, while other work is idle. Reproducing the historical
first study requires its archived runner, with the ordering limitation above.
Do not compare profile-on timing to the release samples.

## Native audit boundaries

The audit checked x64/A64 helper snapshots, argument/frame conventions,
forwarding joins, guest NZCV preservation, loop budgets, callback exits,
physical mappings and generation/translation invalidation. Full native
functional gates plus focused positive-execution fixtures support the stated
changes; event/fallback totals alone are never counted as native instructions.
The A64 odd-PC LDM exit guard remains a conservative potential simplification,
not a new default optimization, because no physical A64 cost was established.
No A64 guard, paired-load, TLB or chaining candidate was silently adopted.

Fresh GCC and A64 CMake builds require C23, enable tests and CPU profiling, and
disable IPO. The baseline targeted native suite passed 9/9. The mapping series
passes full x64 and A64 JIT differential gates plus its new targeted cases.
Existing compiler warnings are retained in logs. The first attempt to build the
new certificate target preceded CMake reconfiguration; its saved no-rule error
is an infrastructure ordering error, followed by the successful configured
build. Historical `fastmem-certificate-*` 12-case and wrong-write-proof logs
cover the rejected endpoint inference; the 76-case `range-certificate-*` matrix
is the stronger final proof. The initial 132-case and incorrect non-RAM fixture
snapshots are preserved as `fastmem-initial-fixture.c` and
`nonram-initial-fixture.c`; their CORE_UNDER_TEST include can target the named
source exports. The full Switch permission
mock was not repeated because these changes do not require re-testing its
entire expensive platform mock. Final combined-source functional gates belong
to the parent integration record; no physical device, release or merge was
performed by this worktree.
