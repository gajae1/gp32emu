# Native I/O stores and translated non-RAM code

The two common native word-I/O handovers now call
`arm920t_cpu_code_write(c, pa, 4u)` after either the complete-span direct store
or the optional/ordinary callback store. All callback-sensitive decoded fields
are already captured. Writeback follows the store notification, and the
existing generation/epoch fence rejects continuation when the write invalidated
translated instructions. Guest register, callback and writeback order is
unchanged.

This is wiring for the separately audited common non-RAM code tracker. By
itself the old common notifier still ignores a device-window physical address;
apply the sibling rare-region tracking fix
`89184ed82f5982d075420e1b0b5a81be444d0c35` as well. The exact common source
used here has SHA256
`7438796c34eab2a39a4bffb2c31274e214950d5e29789e43de10dda38a64a733`.
`nonram-final-combined-sources.json` records the assembled source and fixture.
The full combined core SHA256 is
`8e390d734ecd6195a40044341ed841af092492104c3267e53831907b3bd7440c`.

The independently compiled final fixture passes 10,596 cases with GCC13 x64,
Clang18 x64, and GCC13 A64 under QEMU: 476 raw, 5,060 cached, and 5,060 native
mode cases. Categories include 960 replacements, 72 active traces, 216 external
replacements, 60 ordinary-data controls, 24 halfword-boundary cases, 9,072 clipped
external spans, and 192 public-dispatch BL cases. Positive execution counters
report 4,564 cached and 2,076 native instructions; mode labels alone do not
prove native execution. All builds use C23, -O3, NDEBUG, XOPEN_SOURCE=700, and
CPU profiling enabled. Build/run logs accompany this note.

The previous 1,308-case frozen fixture is retained in `nonram-combined-*`
evidence. It reported 96 cached I=1 expectation failures and zero native
failures on both backends because it assumed every cold non-RAM PC was admitted
to the decoded cache. Existing public dispatch deliberately fetches such a PC
raw unless a translation already exists. The corrected fixture explicitly
prepares its cached target and adds public-dispatch BL-stitched leaves, keeping
the architectural value oracle intact. The common production change did not
need a fix for that fixture-admission mistake.

No physical A64 timing is inferred from these functional runs. The common
notifier returns immediately with I-cache enabled; ordinary GP32 I/O does not
invalidate standard RAM code merely because BIOS code has been translated.
The parent integration measures the combined notification costs separately.
