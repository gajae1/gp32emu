# Read-only review of the final external-write/HLE cost drivers

The JIT reviewer inspected the final drivers at integrated source/test freeze
`865f88e2d095f71ac80935bd775a38f3b8985246` before the root agent began its final
sequential timing window. No execution, accounting, or pair-order blocker was
found. This review did not change a driver, add a performance variant, or run a
timed sample. It is separate from the rejected predicate studies.

## Work and clock boundaries

`scripts/bench_external_writes.c` materializes a host RAM store, optionally
notifies the external-write gateway, resets two guest registers, and dispatches
exactly two guest instructions on each iteration. Its independent final oracle
checks the retirement total, cycle delta, literal MOV result, BX destination,
CPSR, and all 8 MiB of RAM. Setup, the initial translation, 64 warmup iterations,
and full-RAM validation are outside the clock. With profiling enabled, a native
sample must have an actual native instruction count equal to the retirement
total. Cache event/fallback counters are not substituted for that count.

Its `ns_per_call` therefore measures the complete **store + optional notification
+ register setup + two-instruction dispatch** iteration. It is not an isolated
notification-function latency. The no-notification control retains the store
and subsequent guest work. The same-page and different-page data cases leave
both guest instructions unchanged. Scenario 4 writes data at `code + 8` while
notifying the unchanged instruction at `code`; it measures conservative code
revalidation, not the cost of compiling a new instruction stream. The 128-byte
notification case still performs one 4-byte host store and reports its broader
notified span explicitly.

`scripts/bench_hle_outputs.c` measures word, byte, 32-byte C-string, and 256-byte
C-string output gateways. It prepares and warms the same machine first, then
executes **zero guest instructions inside the clock**. It verifies every RAM
byte and unchanged guest cycles/registers outside the clock. Strings include
the terminating NUL in their reported 32/256 bytes. Both destinations contain
data: one is on another page and one shares a translated code page without
overlapping an instruction. The compiler barriers in both drivers retain the
host writes; they are not host memory fences.

The HLE coverage build also verifies no CP15-cache invalidation event for these
non-code outputs. That event check does not represent native instruction
coverage; this component deliberately has no timed guest execution.

## Runner and build checks

Both Python runners choose pair order directly from each case's repetition
index. Thus every individual case receives AB, BA, AB, BA, and so on. There is
no rotating-case index that can cancel this alternation, as happened in the
first rejected predicate runner. The runners retain raw order-labelled samples
and each pair's percentage change, pin the parent process so children inherit
its CPU affinity, and validate the workload metadata returned by each binary.

Coverage mode requires suppressed clocks and profiling enabled. Timing mode
requires a live clock and profiling disabled. Setup and validation therefore
remain part of process execution but are excluded from `elapsed_ns`.

The build manifests inspected before timing identify clean source snapshots:

- Before: `85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f`.
- After: `865f88e2d095f71ac80935bd775a38f3b8985246`.

Each has four completed builds: GCC and Clang, each with profiling off and on.
`scripts/build_core_write_benchmarks.py` takes production sources and headers
from the selected complete snapshot while using the same final independent
drivers. Both use Release C23, IPO off, and matched per-driver options. The
reviewed file and build-manifest hashes are recorded in
[benchmark-driver-review.json](benchmark-driver-review.json).

## Limits that belong with the result

These are hot synthetic component workloads with MMU disabled. They do not
establish raw-interpreter, cold compilation, MMU-alias, full-game, audio, or
physical AArch64 performance. Because both binaries contain complete before
and after core snapshots, observed changes include all round-five source and
layout effects. The full controls and added nanoseconds belong in the report,
including costs for unrelated-page notifications, unchanged-code notifications,
and newly notified scalar HLE outputs. No speedup should be attributed solely
to one internal helper without a separately matched experiment.
