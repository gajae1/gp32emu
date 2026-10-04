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
and Story of Bug Eyed Monster's direct-load API succeeds but a 1200-frame
no-input sample remains dark. Its existing BIOS-based capture reaches a
visible game scene; the two execution paths are not interchangeable evidence.

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
