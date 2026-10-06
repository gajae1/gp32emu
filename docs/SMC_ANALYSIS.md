# Local SMC executable inspection

Build `gp32_smc_inspect` with `GP32EMU_BUILD_BENCHMARK=ON`:

```sh
cmake --build build --target gp32_smc_inspect
./build/gp32_smc_inspect game.smc new-payload.bin > metadata.json
```

The tool reuses the existing SmartMedia reconstruction, FAT12/16, commercial
GXE/GXC and FXE loaders. It reads the original card without changing it, lists
asset paths/sizes/clusters, and optionally writes the selected executable's
loadable bytes. Output payload creation is exclusive: an existing file is
never overwritten. Asset names are printed as JSON metadata and are never
used as host output paths. The payload is private analysis material, not a
file to distribute with the emulator.

`load_addr` and `entry_addr` give the decoder's proposed memory mapping.
`was_host_decrunched` identifies recognized host decompression; false does not
mean the entire game is unpacked. A small loader may fetch/decrypt further
code at runtime. The commercial loader can also reconstruct headers, so its
output must not be represented as an untouched dump of original card bytes.

For analysis, begin at a validated entry point and track ARM/Thumb changes,
calls and PC-relative literals. Compare observed MMIO or SDK calls with the
core. A constant matching a hardware address is only a search lead; scanning
data as instructions produces false positives. Use an existing post-load
state or a narrow execution trace to establish runtime reachability before
changing emulator behavior. Focus on shared hardware/API behavior rather
than patching individual titles.

Initial local catalog pass: 30/31 images produced a payload. Story of Bug Eyed
Monster failed the current GXC header decoder, while Super Plusha produced a
1232-byte loader rather than a proven full program. These are extraction
limits, not proof of game execution failures. Both need runtime-state-based
analysis. Each local result retains card hash, command, decoder metadata and
payload hash in the private `results/resume119-reverse/` manifest.

The follow-up decoder adds the missing `1020/0903` key profile. Its 96-byte
keystream and 0x800-byte stride (0x100 encrypted bytes per span) were recovered
by comparing the canonical FAT-extracted GXC with BIOS-decoded RAM. All
1,018,812 RO bytes match after decoding. The GXE descriptor begins with
`1020/0001`; the profile label identifies the resolved keystream, not a game
name. Candidate scoring and scatter-header validation remain unchanged.

The local catalog now yields 31/31 payloads. The previous 30 outputs retain
their exact payload hashes. This still does not mean all programs are fully
unpacked or playable through direct HLE: Super Plusha remains a small loader,
and the initial Story of Bug Eyed Monster direct-load sample remained dark
despite successful loading and CPU execution. Its SDK has separate create/open
file entries; the existing-file entry's alternate prologue was not recognized
by the direct-HLE scanner. Recognizing its register/stack and device-validation
sequence restores asset reads without depending on a title or link address.
A 1200-frame no-input sample now reaches a visible dialogue scene. The subsequent
color corruption was traced to HLE mixer buffers placed beside a caller-owned
pointer table, overwriting the game's RGB arrays. Mixer storage now occupies a
separate high-RAM region below the direct LCD pages, with the BIOS's 256-byte
buffer alignment. Known loaded-image/scatter ranges are checked before using
that region. The sample's 256 palette words and RGB arrays now match the BIOS
reference. This is not full direct-HLE compatibility; the two execution paths
are not interchangeable evidence of full playability.

Candidate decryption now checks a 32-byte decrypted scatter header before
allocating and decrypting the full body. Header bounds are checked against
both the available header bytes and the complete payload length. Candidates
that pass still undergo the same full decryption, stride scoring and RW-tail
handling. This avoids copying megabytes for incompatible key rotations.

Local warm-file ABBA samples of the complete loader function:

| Image | Previous median | Header-first median |
| --- | ---: | ---: |
| Astonishia Story R | 498.5 ms | 29.5 ms |
| Blue Angelo | 1250 ms | 16 ms |
| Princess Maker 2 | 5938.5 ms | 22.5 ms |

These are Windows direct-loader/inspection times, not gameplay frame rates or
BIOS/SmartMedia access timing. The normal BIOS path and H700 installation are
unchanged. Private provenance, corpus hashes, timing samples and captures:
`results/resume122-loader/`.

The SDK open follow-up is recorded in `results/resume123-hle/`. A ROM-free
regression exercises relocated entry detection, a near-match rejection, asset
reading and EOF. It fails on the previous scanner and passes on Windows and
H700; the H700 check runs from RAM and does not install a core or change volume.

The follow-up in `results/resume124-display/` also separates real SDK card-detect
SWI 0x11 from the private display trampoline. Card queries ignore incidental
surface-like pointers and report either an extracted virtual card or mounted
SmartMedia. The private trampoline retains its display/flip behavior. New buffer
placement takes effect when initialized; already-corrupted old states are not
reconstructed. BIOS-based execution and its memory allocation are unchanged.

Key choice second opinion (round 165, `results/round165/pinball/`). Stage-1 scoring
samples opcode shapes in the first 128 KiB. For Pinball Dreams that sample is data,
and it preferred 0001/3000 rotation 24 at stride 0x400 (score 1876 against 495 for
the real candidate), which corrupted the scatter header and entry (0x0c00145c). A
zero byte encrypts to the keystream byte, so a correct candidate turns zero padding
back into 0x100 zero bytes, while a stride that is too short also XORs windows that
were never encrypted and destroys their zeros. A stage-1 winner that loses more than
one zero per window on average is now replaced by the valid-header candidate with the
most all-zero windows; the 1001/3500 profile (rotation 0, stride 0x1400 for Pinball)
is tried only there, because in the main table it flips ASR's stage-1 winner. Of the 40 unique
local cards (50 files) only Pinball's payload changes (entry 0x0c00005c); its decoded
read-only image equals the BIOS-decoded RAM at game entry byte for byte (2003-05-21
BIOS). Left unchanged on purpose, because output identity for the other cards was a
requirement: against BIOS-decoded RAM the stage-1 picks for ASR (true key 0001/3000,
rotation 32), Little Wizard EU (0001/2500, rotation 0) and Princess Maker 2 (0001/3000,
rotation 0) still differ in 5216, 2432 and 5344 bytes, and Super Plusha (true key
1001/3500, rotation 24, stride 0x1400) still decodes as the 1232-byte loader.
ASR, Princess Maker 2 and Little Wizard EU also stall in direct SMC boot (ASR
spins at 0x0c0742dc, Princess Maker 2 leaves SDRAM, Little Wizard EU stays black
near 0x0c086fb0); fed the BIOS-matching payload through `gp32_headless --fxe` they
got past those points in a 300-frame check (not played further). Whether the same
evidence ranks their true keys first has not been tested.

Round 166 (`results/round166/gxc-keys/`) replaced that rule with the direct
measurement.  A zero plaintext byte encrypts to the keystream byte, so a card
shows its own key wherever its padding is zero; every candidate with a valid
scatter header is now ranked by the zero bytes its decryption restores minus the
zeros it destroys, and the opcode score only breaks exact ties.  That fixes the
three cards above (ASR 0001/3000 rotation 32 stride 0xc00, Little Wizard EU
0001/2500 rotation 0 stride 0x2c00, Princess Maker 2 0001/3000 rotation 0 stride
0x1c00) and lets 1001/3500 become an ordinary table entry.  The same ranking
replaced the GXE descriptor table that selected the stripped payloads: Dungeon &
Guarder (KR) decodes with 1001/1110 at stride 0x1000 and Dyhard Infinity (KR)
with 1002/1127 at 0x1800, the same keys as their European releases, where the
descriptor named a key the table did not hold.  31 of the 33 cards with a GXC
now decode byte-identically to the BIOS read-only image at game entry.  Open:
Hany and Super Plusha hold payloads larger than the card file (packed images the
direct loader does not decrunch), and ASR's direct boot still stalls at
0x0c008a9c in a guest task-list walk even with a BIOS-identical decode.
