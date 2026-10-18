# BIOS replacement lifecycle regression

The two fixtures test separate properties:

| Fixture | Before | After |
| --- | --- | --- |
| `gp32_bios_reload` | 12 cases, eight stale-instruction assertion failures | 12 cases, zero failures |
| `s3c2400_bios_load` | 12 checks, partial-read mutation and overlapping-source failures | 12 checks, zero failures |

The public reload test uses synthetic MOV/BX instructions, file/buffer APIs,
raw/cached/native execution and both CP15.I values. A successful reload must
replace the instruction without resetting architectural registers or cycles.
It also checks the erased tail and rejected oversized input. Native-enabled
runs require positive native instruction coverage.

The SoC test wraps `fread` with a fault injector that performs a real partial
read before the loader detects failure. It compares every BIOS byte afterward,
then checks a successful file, a source slice inside the current BIOS mapping,
and the existing empty-file erase behavior. The test never simulates a
concurrent firmware writer or uses a game ROM.

`before-sources.json` and `after-sources.json` identify the exact checkpoint
around the BIOS change, based on local `c871331890c23c296cf8b25be90971b73f8b3546`.
The fixtures are byte-identical across that comparison. The later PWM changes
alter the surrounding core source; final integrated C23, UBSan and focused
ASan tests run the same fixtures against the combined source.

After building the two CMake targets, run:

```sh
ctest --test-dir /path/to/fresh-build --output-on-failure -R '^(gp32_bios_reload|s3c2400_bios_load)$'
```

CTest supplies independent temporary firmware paths. Full commands, compiler
flags and hashes for the final source are in
[verification](../verification/README.md) and [ASan](../asan/README.md).
The optional reproduction history retains the original before/after source
objects; its intermediate commit IDs are audit identities, not public branch
names or individually recommended release states.

The fix stages ordinary file reads before modifying live BIOS, uses `memmove`
before erasing an overlapping input's tail, and fully invalidates translated
instructions after successful public loads. Allocation/short-read failure
cleans temporary resources and preserves the previous firmware.
