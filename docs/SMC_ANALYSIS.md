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
