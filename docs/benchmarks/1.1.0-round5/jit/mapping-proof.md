# Native direct-mapping proof correction

The final native routing uses an explicit, optional `arm_bus_t.fastmem_excludes`
callback. It does not infer the absence of a range from one rejected access.
The initial native correction commit `479bd2b590c0c989859db066f8783bd2446b9c55`
contained such an inference and is retained as a rejected intermediate in the
local audit history. Apply its follow-up correction together with it.

## Defects and contract

The raw interpreter probes the complete requested fastmem span before a bus
callback. The cached interpreter and both native backends instead selected I/O
callbacks directly for physical addresses in `0x14000000..0x15ffffff`. Generic
buses can authorize direct mappings there. The independent sibling regression
has 2,478 cases: 826 raw, 826 cached, and 826 native; the original baseline has
1,864 failed assertions, split 0 / 932 / 932. The initial native fix changes the
native word handover; the sibling `eae2f78` commit fixes the six portable memory
helpers, including all native cold subword/block paths. Their temporary combined
source passes 2,478 cases with 1,576 positively counted native instructions on
x64 and A64. See `fastmem-combined-sources.json` for that exact source snapshot.

Review then identified that `open_bus_valid` only limits where RAM mappings can
exist; it does not promise acceptance of every access width and permission at
every point inside RAM. Rejecting one byte at `0x0c800000` therefore cannot
exclude an accepted word at a later RAM address. The A64 open-bus shortcut
already made this inference in the public baseline. The first native I/O fix
had independently repeated it for reads and writes; checking both directions
was necessary but insufficient.

The expanded 76-case native regression uses contiguous physical RAM with read
and write permissions independently enabled, a rejected first extension word,
and permissions accepting words while rejecting bytes. It checks both
`0x14001000` (device window) and `0x12001000` (extended RAM outside that window),
MMU off and a prefilled alias, rotated LDR, STR, untouched registers and flags,
callback selection, direct store destination, and exactly 228 native retired
instructions. One copied CPU/bus also moves through absent, read-only,
write-only, read/write, and absent mapping states, with a required flush and
recompilation between changes.

| Source | Cases | Failed assertions | Architectural/callback/store failures | Native instructions |
| --- | ---: | ---: | ---: | ---: |
| Intermediate `479bd2b`, x64 | 76 | 102 | 28 | 228 |
| Intermediate `479bd2b`, A64 | 76 | 120 | 36 | 228 |
| Original `85d36d5`, A64 | 76 | 186 | See raw log | 228 |
| Range certificate, x64 | 76 | 0 | 0 | 228 |
| Range certificate, A64 | 76 | 0 | 0 | 228 |

The failing total includes explicit new certificate-query/probe assertions.
The intermediate x64 breakdown is 60 missing certificate queries, 14 probe
assertions, 8 register errors, 12 callback-selection errors, and 8 direct-store
errors. A64 has 60, 24, 16, 12, and 8 respectively. Of the original baseline's
failures, eight register errors occur at the non-I/O extended RAM address;
those independently establish the pre-existing A64 open-bus defect.

The before-core compilations use the expanded test and appended bus header on
both sides so the old core can be checked against the new certificate-aware
fixture. The before `arm920t.c` and included backend code are unmodified exports
of the named commits. The header addition does not change earlier bus entries.

## Proof and behavior after the correction

A nonzero `fastmem_excludes(user, address, bytes, write)` result promises that
fastmem rejects every nonempty subspan wholly inside that complete physical
interval for the requested permission until the usual mapping-flush boundary.
Zero or a missing callback is unknown. Absence of fastmem itself trivially
proves the absence of every direct mapping. The query must be side-effect-free.

Both native backends ask about the entire 32 MiB device window, independently
for reads and writes. Only that certificate selects the original callback-only
word helper. Every other bus retains the mapping-aware native helper, which
probes the exact aligned four-byte physical span before invoking the ordinary
or optional I/O callback. Snapshot, word rotation, writeback, callback ordering,
ABI and continuation fences are shared with the original helper.

A64 additionally asks for read-mapping absence throughout the full physical
range after the standard 8 MiB RAM window before enabling its open-bus constant.
The existing open-bus contract rules out extra mappings below/outside BIOS and
contiguous RAM; the complete exclusion rules out RAM extensions, including
permission holes and width-dependent access. BIOS access still requires the
complete read-window pointer proof.

S3C2400 implements the certificate with an interval-overlap check against its
live BIOS/RAM extent. It reads `s->ram_size` for every cold-compile query. A
snapshot boolean in `s3c2400_get_bus` would be unsafe because standalone state
loading can replace and grow that allocation while a CPU retains a copied bus.
The separate 88-case SoC test checks interval boundaries, read/write BIOS
permissions, invalid/overflow ranges, copied-bus 8 MiB -> 16 MiB -> 8 MiB state
loads, and an allocation extending into the I/O window. It passes on x64 and
A64. The generic native test separately proves that the CPU re-queries after
each required mapping flush.

## Scope and costs

The bus field is appended; previous members keep their offsets. No public CPU
state or save-state format changes. Ordinary S3C2400 I/O retains the callback-only
runtime path; the new test explicitly verifies zero device-address fastmem
probes where a valid certificate applies. Generic word I/O now pays one exact
mapping probe when absence cannot be proved. Certificate callbacks run during
compilation, not every guest instruction. A64 buses which supplied only the old
open-bus promise now use a checked helper for addresses outside normal windows
until they supply an actual exclusion certificate.

No physical A64 performance claim follows from QEMU test durations. No whole
Switch permission mock was re-run: the changed host helpers, mapping contracts,
and SoC certificate are covered by the targeted cross-platform tests and full
native differential gates. The common rare-region self-modifying-code notifier
is a separate follow-up described in `nonram-store-notifier.md`; it is not
implied by this mapping fix.
