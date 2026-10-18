# Reconstructing isolated measurement sources

These patches describe source variants used for individual measurements.
They are **not** extra changes to apply to the current review branch.
The public starting commit is
`b141220cd5fd9d628305133fc963ed86a30f7e41`.

For the Thumb pair, use a disposable checkout at that commit, apply
`thumb-baseline.patch`, and preserve `src/arm920t.c`. Its SHA-256 must equal
`3f2fff84a5de7e53ee5f02319f3f54485fcf0e9188de3a55e698d3d4d9b51324`.
Apply `thumb-candidate.patch` to obtain the after source, hash
`9b6c89ddd91e5261bc1a47cd3ae8b8d1d652c3ed70bcdc1dcc330ba15344a656`.
Compile the same `scripts/bench_thumb_transfers.c` against either full source
and use the configuration in [the transfer report](../README-thumb-transfers.md).

For the HLE pair, separately apply `hle-zero-support.patch` to the starting
commit. Preserve the before `src/gp32.c`, then apply `hle-zero-candidate.patch`.
The source hashes and shared-object linkage are described in
[the zeroing report](../README-hle-zero.md).

For the x64 forward-branch pair, separately apply
`x64-forward-baseline.patch` to the starting commit. The before
`src/arm920t.c` must hash to
`ec5e28cfe3f6a311d2bbe6a0233c44e2c0cac83d40995d9c46ca72a1232c4d5d`.
Apply `x64-forward-candidate.patch` to obtain the after source, hash
`4156b8a7381c660480f0e6ef265639019182c39deb12e1573b640cec1c3eac68`.
The candidate patch also moves the existing A64 eligibility helper into the
shared source. Use the unchanged driver and flags in
[the forward-branch report](../x64-forward.md).

All six source hashes were independently reconstructed from the public base;
[the reconstruction record](../evidence/isolation-reconstruction.json)
includes the patch hashes and exact results. Local integration commit IDs
are not required to apply these exported patches.

The independent experiments precede other integration changes. Their source
hashes intentionally differ from the final combined core, whose functional
validation is recorded separately. A local integration commit and an isolated
agent commit can also have different trees; neither is a substitute for these
exact measured source bytes.
