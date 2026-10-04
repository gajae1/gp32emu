# Fixed-capacity two-way block cache

2026-10-05, baseline `e89e85c`. The ARM block table still contains 16,384
headers and operation rows, now arranged as 8,192 two-way sets. Colliding hot
PCs can coexist without growing those allocations. PC, generation and fetch
epoch checks remain mandatory. Native arena retirement and guest code-change
invalidation are unchanged. This applies to all titles; there are no ROM checks.

The initial worker candidate scanned both ways twice and regressed H700
Astonishia title throughput from 188.3 to 180.3 benchmark frames/s (~4.2%).
It was not promoted. The integrated version preserves the original first-way
hit path and consults the second way only after a miss. A follow-up at stable
1.512 GHz measured 188.4 baseline versus 188.2 candidate; the cold-start samples
were excluded from that comparison because the stock governor was ramping.

## Bounded H700 evidence

Final source, stock conservative governor, no volume/mixer/config changes.
Each scene reloaded the same native state and inputs, warmed up for 1,200
emulated frames, then measured 1,200. These are headless profiling throughput
figures, not screen refresh rates or a claim of full gameplay acceptance.

| Scene | Baseline / candidate frames/s | Translations | Table conflicts | Arena recycles |
| --- | --- | --- | --- | --- |
| Blue Angelo slot 0, walking left | 75.688 / 76.785 | 10,463 / 3,983 | 5,342 / 527 | 1 / 0 |
| Astonishia Story R title | 186.200 / 186.927 | 15,059 / 11,612 | 3,690 / 14 | 0 / 0 |

The small throughput differences are insufficient to establish an FPS gain.
The reproducible benefit is less translation work and arena pressure: Blue's
arena high-water mark fell from 67,046,088 to 36,753,548 bytes. Guest cycles,
PC, CPSR, clock, PCM frame count, video hash and PCM hash matched in each pair.
Eight protected device-file hashes matched before/after. Installed core was
not replaced. This does not establish a fix for the NPC dialogue pause.

Final PC JIT differential and arena-recycle checks pass. The initial two-way
candidate also passed both on H700; the changed first-way fast path then passed
the native recycle check and the two real-state replays above. The fixture forces
three colliding PCs, searches the correct way and rejects a missing native
block explicitly rather than returning an unrelated set base.

Final Windows x64, Linux/H700 AArch64 and Android arm64-v8a/armeabi-v7a libretro
cores build in C23 mode. Android runtime and physical input/audio remain
unverified. Local raw evidence and reproducible device runner:
`F:/GP32/results/round135-cache/` (`device-ab.json`, `device-ab-fast.json`,
`device-ab-warm.json`, protected manifests and `release-builds.json`). Earlier
worker PC comparisons are under `round133-parallel/jit-cache/`.
