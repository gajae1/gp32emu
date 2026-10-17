# CPU writes to decoded ARM code

This follow-up fixes a reproduced instruction-coherency error when CP15 c1.I
is clear. It is a correctness change with measured execution costs. No affected
commercial game, game-speed improvement, or physical-device result is claimed.

## Failure and architectural scope

With c1 at its reset value `0x70`, a RAM function contains `MOV r0,#0x11` and a
return. After that function has executed, a guest `STR` replaces the MOV with
`MOV r0,#0x22`. The next indirect call returns `0x11` in both cached portable
and native execution, although RAM contains the new instruction. The traced raw
interpreter returns `0x22`. Disabling native JIT alone is insufficient as an
oracle because the portable ARM path also executes cached decoded blocks.

ARM920T DDI 0151C, sections 4.2.2–4.2.3, says that an instruction fetch with the
I-cache disabled ignores its contents and reads memory. The branch-to-patched-
function regression exercises that requirement. The emulator's existing
instruction-at-a-time execution model also requires ending an active decoded
trace before it executes an instruction that the just-retired store changed.
This does not establish cycle-exact ARM pipeline or instruction-cache behavior.

Primary reference: [ARM920T Technical Reference Manual, DDI 0151C](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb).

## Change

- Raw ARM/Thumb writes and classified word, byte and halfword stores notify the
  CPU after the bus write. SWP and block transfers use those same accessors.
  The existing continuation fence observes the changed cache epoch after the
  complete guest instruction, including all transfer lanes and writeback.
- Native x86-64 and AArch64 stores test a physical RAM watch before any guest
  mutation. A hit enters the existing checked instruction/access helper; an
  ordinary data write retains its direct RAM path. A block store checks every
  instruction word in its complete span, including interior words. A64's
  two-page STM path checks each independently resolved physical fragment
  before any transfer, using its actual dynamic word count.
- The CPU watch bitmap has one bit per 4-byte instruction word of the 8 MiB
  physical RAM window. A padded 64-bit bitmap load, followed by a shift and
  a mask, tests all 1–16 words of an aligned STM fragment without a loop.
  The final padding word makes that load safe at the end of RAM. Scalar word
  stores check one bit; a halfword can touch two instruction words.
- The bitmap uses 256 KiB plus four bytes of padding per CPU. The existing
  4 KiB bitmap and conservative external DMA/HLE invalidation policy are
  retained. The CPU bitmap is separate so normal data sharing a code page,
  or even the same 64 bytes, does not repeatedly revoke unchanged code.
- Fetch collection records physical addresses using the instruction mapping.
  Successful block revalidation also records every touched watch, including
  the interior of a long span. Therefore a byte-identical I-TLB remap cannot
  leave the newly mapped physical code page unwatched when a D alias writes it.
- Native watch guards are generated only when CP15.I is clear. Guest c1 writes
  already invalidate the compiled generation; a real MCR transition test
  protects that assumption. I-enabled execution retains the existing guest
  maintenance policy and adds no native store guard.

The bitmap is appended to the runtime CPU structure. CPU wire/save-state formats
are unchanged. Writes beginning just below RAM are clipped to their actual RAM
overlap; this preserves the existing split-byte `s3c2400_write16` behavior for
an unaligned halfword at `RAM_BASE-1`.

The native guards preserve each path's pending address and writeback operands.
On x86-64 the store guard uses EAX/ECX/R11 while preserving EDX/R9/R10; normal
non-ALU emission already discards the previous EAX forwarding assumption. On
A64 the scalar guard preserves w0/w2/w11, and the STM window uses x6/x7/x17,
preserving w2/w4 physical offsets and w5's split count. The second fragment's
dynamic count in w16 survives until its mask is formed. Guard/fixup arrays
accommodate the maximum number of exits for the respective transfer shapes.

## Tests

`tests/arm_cpu_code_write_test.c` contains 88 scenarios covering portable and
native execution, word/byte/halfword stores, SWP/SWPB, STM, 4 KiB and 64-byte
boundaries, current traces, raw ARM and Thumb writers, separate I/D MMU aliases,
short and 48-instruction remap proofs, unchanged data in the same 64 bytes,
guest cache maintenance, and a guest MCR changing I from 1 to 0. Interior STM
words are checked on both sides of a page boundary, including adjacent virtual
pages backed by physically separated RAM pages. A full 16-word STM at the end
of RAM checks the padded window's final starting position. Registers, flags, cycles
and RAM are compared with the raw interpreter where the cache model agrees.
The I-enabled cases separately check the established maintenance behavior.

The original 44-case regression failed 72 assertions before the fix. Adding
the RAM-entering halfword case exposed four further mismatches before clipping
was implemented. The initial 4 KiB CPU guard failed the four same-page data
no-invalidation checks. A test mutation that records only the first and last
64-byte watch of a long revalidation span fails four assertions in the remap
tests; recording the whole span passes. The 64-byte CPU prototype fails four
new no-invalidation assertions for unrelated data in the same 64 bytes. Merely
changing its watch shift to 2 without fixing the STM range checks fails eight
x86-64 and twelve A64 assertions. These failures motivated the complete-range
masks; checking only a transfer's endpoints is insufficient at this granularity.
An A64 mutation that checks physical page 0's neighbor instead of the resolved
second page fails the new separated-page native case with two assertions
(`r0=0x11` instead of `0x22`); the unmodified implementation passes.

The selected exact-word implementation passed all 88 focused cases on x86-64
and A64 under QEMU in C23-required Release configurations with profiling
enabled. The earlier 64-byte prototype also passed the then-current 41-target
CTest suite on both architectures. Combined review-branch suite results are
recorded in the review document; the final branch includes independently
reviewed fixes in addition to this one. Existing compiler warnings remain;
these builds do not establish warning-free compilation. QEMU establishes
executable A64 semantic coverage, not performance on an ARM device.

```bash
cmake -S . -B build-cpu-write-review -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DGP32EMU_BUILD_TESTS=ON \
  -DGP32EMU_CPU_PROFILE=ON -DGP32EMU_REQUIRE_C23=ON
cmake --build build-cpu-write-review
ctest --test-dir build-cpu-write-review --output-on-failure
```

## Measured tradeoffs and rejected prototypes

The selected implementation trades memory and some store throughput for
correct instruction visibility. The rejected coarse variants make that cost
depend strongly on where a game puts its variables, so their better memory
footprint does not justify using them here.

Three synthetic workloads share the same checked benchmark fixture:

- Scalar: `ADD r0,r0,#1; STR r0,[r7]; B loop`, with the data in a different
  page or at code+`0x20`, a different instruction word in the same 64 bytes.
- STM16: the same three-instruction loop with `STMIA r7,{r0-r15}` instead of
  STR. The base is not written back. All 64 written bytes avoid the actual code,
  including the placement at code+`0x20`.
- Wide: five instructions in BIOS increment r0, write RAM, advance r7 by 4 KiB,
  wrap at the 8 MiB boundary, and branch. It visits 2,048 RAM pages. The data
  stores touch 2,048 host cache lines; the exact-word bitmap accesses also
  cover 2,048 lines, compared with 256 lines for the 64-byte bitmap, assuming
  64-byte host cache lines. This tests a larger metadata working set.

Each sample retires 30,000,000 guest instructions after a 30,000-instruction
warmup and checks the final r0, RAM, retirement count, generation and epoch.
The exact-word candidate requires an unchanged epoch for every placement.
The baseline, rejected 64-byte prototype and exact-word candidate are
interleaved, five samples each, pinned to Linux CPU 0 on a virtualized AMD
EPYC 9V74 host. All use GCC 13.3.0, `-O3`, `-std=c2x`, and CPU profiling.
Baseline is commit `86eb4f98ad893b99eb5f94ebb5b9093252475611`.

The table shows native I=0 medians in nanoseconds per guest instruction, not
per store lane. A positive percentage is slower than the original baseline.
These are host microbenchmarks, not whole-emulator FPS.

| Workload | MMU | Data placement | Baseline | Rejected 64-byte | Selected 4-byte | Selected cost |
|---|---|---|---:|---:|---:|---:|
| STR | Off | Other page | 2.1158 | 2.4150 | 2.3910 | +13.01% |
| STR | Off | Same 64 bytes | 2.1125 | 13.2249 | 2.4273 | +14.90% |
| STR | On | Other page | 2.5438 | 2.7204 | 2.6898 | +5.74% |
| STR | On | Same 64 bytes | 2.5915 | 17.1964 | 2.8490 | +9.94% |
| STM16 | Off | Other page | 2.9803 | 3.5243 | 3.2421 | +8.78% |
| STM16 | Off | Same 64 bytes | 3.1431 | 37.1369 | 3.3411 | +6.30% |
| STM16 | On | Other page | 3.6759 | 4.4441 | 3.8609 | +5.03% |
| STM16 | On | Same 64 bytes | 3.5746 | 41.4876 | 3.9663 | +10.96% |
| Wide STR | Off | Whole RAM | 1.3529 | 1.5919 | 1.5726 | +16.24% |
| Wide STR | On | Whole RAM | 1.6340 | 1.8596 | 1.8712 | +14.52% |

The exact-word candidate removes the roughly 6× scalar and 11× STM false
positives without materially worsening the wide-write result relative to
the 64-byte prototype on this host. It does not eliminate the ordinary
I-disabled store guard cost. I-enabled native controls across all three
workloads vary from -1.95% to +0.76%, with no emitted watch guard. I-disabled
portable controls cost +0.31% and +7.16% (MMU off/on); raw ARM costs +7.24% and
+4.95%. Code layout, scheduling and measurement variation remain relevant;
small negative numbers are not treated as optimization gains.

The allocation difference is explicit. The following sizes come from the
benchmark binaries, with profiling enabled, before integrating the separate
writable-fastmem-pointer fix:

| Version | CPU watch array | `sizeof(arm920t_t)` | CPU allocation increase |
|---|---:|---:|---:|
| Baseline | 0 | 347,840 bytes | 0 |
| Rejected 64-byte | 16,384 bytes | 364,224 bytes | 16,384 bytes |
| Selected 4-byte | 262,148 bytes | 609,992 bytes | 262,152 bytes |

The selected array is exactly 256 KiB plus a four-byte padding word; structure
alignment contributes a further four bytes here. It uses 245,768 more allocated
bytes than the 64-byte prototype. Full invalidation clears this larger bitmap,
so workloads repeatedly writing actual decoded code can also pay more clearing
cost. The benchmark does not claim to measure that workload, ARM device cache
behavior, or a particular game's memory budget.

The earlier 4 KiB prototype invalidated on each write anywhere in a code page
and failed the same-page data invariant. An initial single exploratory timing
for a code+`0x200` variable was 2.7828→12.9818 ns per guest instruction; this
was a rejection signal, not a repeated benchmark estimate. The subsequent
64-byte prototype passed that placement but still invalidated for code+`0x20`.
Its original 200 repeated samples are preserved in
[the historical measurement file](measurements/cpu-code-writes-x64.json),
including that roughly 6× result. The final three-version comparison retains
all 360 individual samples and their checks in
[the exact-word measurement file](measurements/cpu-code-write-words-x64.json).

### Reproduce the microbenchmark

The C benchmark deliberately reuses the existing synthetic test bus and
contains no firmware or game image. `CORE_UNDER_TEST` can name the CPU source
in a separate baseline checkout, whose sibling A64 include will be selected
with that source. Compile both binaries with identical compiler options.

```bash
cc -O3 -std=c2x -D_XOPEN_SOURCE=700 -Isrc -Iinclude \
  scripts/bench_cpu_code_writes.c -o /tmp/cpu-write-after
cc -O3 -std=c2x -D_XOPEN_SOURCE=700 -Isrc -Iinclude \
  '-DCORE_UNDER_TEST="/absolute/baseline/src/arm920t.c"' \
  scripts/bench_cpu_code_writes.c -o /tmp/cpu-write-before
python3 scripts/bench_cpu_code_writes.py \
  /tmp/cpu-write-before /tmp/cpu-write-after cpu-write-results.json
python3 scripts/bench_cpu_code_writes.py \
  /tmp/cpu-write-before /tmp/cpu-write-after cpu-write-extended.json --extended
```

The default Python runner repeats all three execution modes, both I states and
MMU states, and the three native scalar placements. `--extended` selects the
bounded 24-case STR/STM16/wide comparison. `--intermediate /path/to/binary`
adds a third version, as used to preserve the rejected prototype comparison.
It retains every sample, checks each process's exit status and rotates the
binary order. On Linux it uses the first allowed CPU unless `--cpu` selects
another. Direct C-benchmark arguments are execution mode (0 portable, 1 native,
2 raw), I, MMU, guest-instruction count, placement (0 other page, 1 same page
at +`0x200`, 2 same 64 bytes at +`0x20`) and shape (0 STR, 1 STM16, 2 wide).
The wide loop requires placement 0 and an instruction count divisible by 5;
the other loops require a count divisible by 3.

## Remaining limits

No physical Android, iPhone or retro handheld was used. No ROM library was
scanned to establish how often games disable I or rewrite decoded instruction
words. Watches remain conservative for a previously decoded word and do not
compare old/new instruction bytes; writing an identical value can still revoke
code. Cached/uncacheable-region attributes when I is enabled,
FCSE, WFI and a detailed hardware I/D cache model are separate work. The new
metadata covers the GP32 8 MiB RAM window used by decoded ARM/native code;
external-write behavior and general bus alias contracts are unchanged.
