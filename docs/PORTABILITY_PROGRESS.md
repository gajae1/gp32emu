# GP32 ARM / RetroArch progress (2026-10-03)

This is a development checkpoint, not a full compatibility or performance claim.
Base: fork `gajae1/gp32emu`, commit `9ca3a72`. Upstream is
[gameblabla/gp32emu](https://github.com/gameblabla/gp32emu).

## Hardware and scope

The GP32 uses Samsung S3C2400 / ARM920T, 8 MiB SDRAM, 512 KiB BIOS and a
320x240 landscape display (native LCD memory is portrait). Games program their
own clocks; a fixed 133 MHz assumption is not a useful benchmark budget.
The [MAME GP32 driver](https://github.com/mamedev/mame/blob/master/src/mame/gamepark/gp32.cpp)
is a hardware reference, not a demonstrated compatibility oracle. No new MAME
code was copied during this work.

The tested handheld is an H700 / Cortex-A53 aarch64 device running SpruceOS on
Linux 4.9.170, with a 720x480 panel and 1 GiB-class RAM. The UI identifies its
platform as AnbernicXX720480NoStick. Exact retail model was not assumed from
"RG SP". SSH and USB ADB both work. Do not package BIOS or ROMs.

## Implemented

- CMake shared libretro builds for Windows, Linux aarch64 and Android arm64-v8a /
  armeabi-v7a; Android uses the required `_libretro_android.so` filename.
- H700 cross toolchain using Zig 0.13.0 and glibc 2.17.
- Cached LCD line calculation, invalidated on peripheral ticks/register writes/reset/state load.
- LCD scanout specializes the pixel format once per frame and reads contiguous
  in-RAM DMA words in one operation, retaining the original wrap/bounds fallback.
- Fixed-point polling-loop acceleration with per-access stability checks and
  exact instruction budgets; MMIO stack writes and changing-state loops excluded.
- AArch64 native block backend with a post-invalidation DSB/ISB completion
  barrier (required on the tested Cortex-A53 with the linked cache-flush helper).
- AArch64 fast MMU-hit translation using one block-local TLB base register and
  short strided loads; a packed derived TLB mirror for the portable C path.
- Libretro audio partial acceptance retained across calls, silent-frame pacing,
  lifecycle resets and a 250 ms bounded queue during sustained backpressure.
- Persisted SmartMedia restored on later loads, and flushed on content replacement;
  corrupt persisted images cause an explicit load failure.
- Additive Spruce GP32 entry using the existing standard launcher and 64-bit RA.

## Evidence so far

Five fixed boot/menu scenes (Blue Angelo, Little Wizard, Her Knights, Dungeon &
Guarder and Astonishia Story R) match the original portable core's video/PCM
hashes, cycle count, PC and CPSR after 2400 warmup + 300 measured frames on
Windows. This does not cover whole games or their most demanding scenes.

On H700, same BIOS/content/input schedule and no manually forced CPU governor:

| Scene | Original portable | Polling optimization | AArch64 native candidate |
| --- | ---: | ---: | ---: |
| Blue Angelo boot/menu window | 12.2 fps | 60.4 fps | 103.6 fps |
| Little Wizard character selection | not separately measured | 9.8 fps | 28.0 fps |
| Her Knights fixed boot/menu window | not separately measured | not separately measured | 52.6 fps |
| Dungeon & Guarder fixed boot/menu window | not separately measured | not separately measured | 152.5 fps |
| Astonishia Story R fixed boot/menu window | not separately measured | not separately measured | 101.7 fps |

These are single final-build measurements. Earlier repeated Wizard runs were
27.0-27.7 fps; the later scanout change has not established a material Wizard
speedup. The benchmark excludes warmup, includes pixel/PCM hashing, and has no RetroArch
render/audio driver overhead. H700 hashes and CPU state match Windows for all
five windows. The first native candidate intermittently crashed in Little
Wizard. Fault context and disassembly exposed an incomplete instruction-cache
flush sequence in the linked helper. After adding the completion barrier,
three consecutive Little Wizard repeats passed with identical results; a
separate sampled-profile run also passed. This bounds the evidence, rather
than establishing crash freedom throughout the game. The corrected native
core is installed in the GP32-specific UI directory with the previous
portable core backed up under `gp32-dev/core-portable-ui-backup.so`.

Focused audio, persistence, polling and native-vs-portable CPU differential tests
have passed on H700. The native memory test uses the full 8 MiB direct RAM window.
Android artifacts have linked and export the libretro API; no Android runtime
has been tested.

## Gameplay-window measurement (2026-10-02)

The `gp32_bench --state` option can start from a native GP32emu state. We
extracted the core payload from a copy of RetroArch's compressed Game Switcher
state, leaving the original save untouched. With Blue Angelo already in its
opening gameplay, 60 untimed plus 180 measured frames on H700 produced 19.46
fps with native JIT and 15.84 fps with the portable CPU. Both executions had
the same final PC/CPSR/cycle count and video/PCM hashes. This window is much
slower than the Blue Angelo boot/menu window above and is now the primary
optimization target. The numbers include framebuffer/PCM hashing but not
RetroArch's presentation cost; they are not a claim of playable full speed.
Sampled profiling of this gameplay window found 588/1432 samples in dynamic
code or shared libraries, 283 in the classified ARM helper, 237 in JIT dispatch,
and 86 in LCD rendering. Sampling includes warmup and is directional evidence,
not a precise per-component time breakdown.

The 2026-10-02 TLB candidate matched the previous core's CPU state, video and
PCM hashes in the Blue Angelo gameplay state and all five fixed boot/menu
windows on H700. Native-vs-portable CPU, polling, libretro audio and persistence
tests also passed on H700. A clean interleaved Blue gameplay comparison at an
observed 1512 MHz CPU frequency measured 35.503 fps for the candidate and
33.242 fps for the prior native build (about 6.8%); it is one matched-clock
pair, not a whole-game speedup guarantee. Earlier runs in the same sequence
started at 480/1200 MHz and are not comparable as an optimization effect.
The candidate's portable execution produced identical gameplay state and hashes,
but its 13.467 fps run had uncontrolled CPU frequency and does not establish
a portable-path speed change. The gameplay window remains below 60 fps even
without RetroArch presentation overhead.

## Spruce integration

The live UI lists GP32 and both test games. Launching Blue Angelo from that list
runs `ra64.h700` with the existing platform configuration and the GP32-local core.
The BIOS menu and Blue Angelo media-selection screen have rendered correctly.
The existing Spruce in-game menu opens with the Menu button. Saving to slot 0
created a state file; after advancing to media selection, loading the
same slot returned to the saved BIOS screen with the frontend's load-success
message and the same RetroArch process still running.
Holding Menu triggered the firmware's existing Game Switcher action, created
an automatic state and returned to its carousel. Selecting Blue Angelo there
launched a new RetroArch process that resumed the saved opening story rather
than returning to BIOS. This verifies one actual Game Switcher round trip. The final scanout build was
then installed and resumed successfully into Blue Angelo gameplay. Its SHA-256
is `e0a22e7d7e1d56943db83d10475c910dd5d841c5d79fc8391753ac3e84308d16`.
The normal UI reload operation was used to refresh the systems list.

The 2026-10-02 candidate replaced only `Emu/GP32/gp32emu_libretro.so` after
backing the previous native core up as `gp32-dev/core-before-20261002.so`.
Installed SHA-256: `71ba3cccb67e5427b631b4e820b39f4c4f29f7f775ce2a2ba93329ed21520efe`.
Launching Blue Angelo from the unchanged Spruce GP32 list resumed its gameplay
in `ra64.h700`; later framebuffer capture showed an advancing animated scene
while the process was still alive. The common platform RetroArch config hash
remained `34a9a68e9425ba800d6383735c7f59fd9fe4d9ac5bf9ad646e8028bba51d4864`.
Speaker sound was not judged by listening, and long-play stability is untested.

No Codex global model/provider settings or common RetroArch configuration values
were manually edited. The platform RetroArch configuration was byte-identical
before and after the native UI save/load checks. The standard Spruce launcher naturally applies its usual
runtime CPU/network/save hooks. The GP32 item defaults to Smart CPU mode.

Remaining: sustained gameplay/audio acceptance and further Little
Wizard optimization. No worst-case-game ranking or full-speed guarantee is established.

## C23 and native JIT follow-up (2026-10-03)

Builds request the C23 dialect where the local compiler supports it. H700
Zig/Clang 18 and Android NDK Clang 19 linked the libretro core in that mode;
legacy MinGW GCC 8 explicitly falls back to C11. `GP32EMU_REQUIRE_C23=ON`
turns that downgrade into a configure failure. The language selection itself
has no established speed benefit.

The AArch64 JIT now emits ordinary MUL/MLA, a safe RAM subset of
LDRH/LDRSB/LDRSH/STRH, and a guarded RAM subset of block transfers. The block
path falls back before changing guest state for PC/user-bank/base-alias,
unaligned, MMU page-crossing, non-RAM and other unsupported cases. On H700 the
expanded interpreter-vs-JIT differential, polling, audio and persistence tests
passed. Blue Angelo gameplay, Little Wizard selection and four other fixed
boot/menu windows matched earlier cycle count, PC/CPSR and video/PCM hashes.

At an observed constant 1512 MHz, one interleaved Blue gameplay pair measured
36.570 fps before the block path and 38.436 fps after it. The matching Little
Wizard pair measured 29.771 and 30.264 fps. These are core-only windows, not
RetroArch display fps or whole-game minima, and remain below the 60-refresh
target. Raw measurements are `F:/GP32/results/block-ab.json`; fixed-scene
hashes are in `F:/GP32/results/block-final-exact.json`.

The resulting core SHA-256 is
`b1f445632280f6bd9f70aea179940c07eb32f38f851e17c194a915f94d023b62`.
It briefly replaced only `Emu/GP32/gp32emu_libretro.so`; the previous core was
backed up at `gp32-dev/core-before-20261003-block.so`. The GP32 platform config
hash was unchanged. A direct SSH invocation of the Spruce launcher started
Blue Angelo in `ra64.h700`, and two framebuffer captures five seconds apart
showed advancing gameplay. The user then reported that the device OS was
corrupted and rebooted it. The current boot's recent kernel log has no matching
OOM, fault or DRM entry, but no log from the incident itself was captured. A
direct SSH launch does not establish normal-menu acceptance. No root cause is
established.

As a precaution, the installed GP32 core was restored to its prior SHA-256
`71ba3cccb67e5427b631b4e820b39f4c4f29f7f775ce2a2ba93329ed21520efe`.
The new binary remains at `gp32-dev/core-after-20261003-block-unverified.so`
for diagnosis. The device was reachable over SSH after reboot, RetroArch was
not running, and the installed core hash matched the restored version. Do not
treat the block-path build as device-stable until a normal UI launch and
sustained gameplay/audio test pass.

## Isolated follow-up build (2026-10-03)

A 16-bpp LCD color lookup table, an x86_64 polling-loop gate extension, and
libretro duplicate-frame presentation for unchanged LCD frames were added.
The table is published once with C11 atomics so concurrent core creation
cannot expose a partially filled table. The changed build passed Windows
`libretro_audio`, `libretro_persistence`, `arm_poll` and `arm_jit` tests and the
same four H700 tests as standalone binaries. The H700 benchmark was run only
as a separate `/tmp` binary; the installed RetroArch core remained on the
previous SHA-256 above.

In one interleaved comparison at 1512 MHz, Blue Angelo gameplay gave 38.327
fps for the follow-up and 38.474 fps for the prior block candidate. Little
Wizard selection gave 30.301 and 30.318 fps for the follow-up, versus 29.963
and 30.342 fps for the prior candidate. All matched final CPU state, cycle
count, video hash and PCM hash. These differences do not establish a useful
game-level speedup. The LCD-only 16-bpp conversion benchmark improved, but it
is not the dominant cost of these scenes. Raw data:
`F:/GP32/results/lcd-and-poll-ab.json`.

A trial AArch64 fast path for ARM-state `BX Rm` also matched the interpreter in
the focused differential test and Blue Angelo's final CPU/video/PCM hashes.
At a constant 1512 MHz its Blue gameplay window measured 38.376 fps against
38.515 fps for the immediately preceding binary, so it showed no benefit in
that workload. The fast path was removed; only its focused `BX` differential
cases remain. Raw A/B data: `F:/GP32/results/bx-blue-ab.json`.

Local evidence: `F:/GP32/results/`. Current native development binaries:
`F:/GP32/artifacts/arm-native-candidate/`; older portable preview:
`F:/GP32/artifacts/portable-preview/`. Commercial assets remain outside the repository.

## Measured leaf-return optimization (2026-10-03)

Optional `GP32EMU_CPU_PROFILE=ON` / `gp32_bench --cpu-profile` counters now
separate native, classified and Thumb work and reset at the measured-window
boundary. They compile out of the normal release. In the Blue gameplay and
Wizard selection windows, Thumb instructions and arena-full failures were
zero. Arena high-water marks were below 2 MiB of the 64 MiB capacity: exhaustion
is a real latent defect, but was not the cause of these scenes' slowdown.

The native arena now recycles at dispatcher translation boundaries before
binding the new block slot. Generation invalidation precedes memory reuse,
including generation wrap. A 128 KiB forced-churn regression failed on the
prior source and passes on x86-64 and H700 after the fix. Eligible polling
blocks no longer generate native code that the dispatcher would never call.

AArch64 LDM can now load an even PC from a guarded contiguous RAM span.
Inlined leaf push/pop also performs the actual stack transfers; it continues
the trace only when the loaded return PC matches the decoded continuation.
Odd targets, MMIO and unsafe spans retain the checked helper. A new aliasing
case exposed an existing x86-64 bug: eliminated leaf stack transfers ignored
a callee's replacement of its saved return address. The x86 backend now
executes those transfers and exits at the real loaded PC. BX r15 also matches
the emulator interpreter's current committed-PC behavior; this is not a
hardware-accuracy claim for that unusual instruction form.

At observed 1512 MHz throughout all Wizard runs, interleaved baseline/candidate/
candidate/baseline measured 30.508 / 37.275 / 37.168 / 30.504 core fps (+22%).
CPU state, cycles, video and PCM hashes matched in every run. The measured-only
Wizard profile (120 frames) reduced native block-transfer helper calls from
8,027,788 to 2,568. Blue gameplay changed 39.048 to 39.024 fps in the one
matched-clock pair; its block-helper calls barely changed. Earlier Blue runs
in that sequence ramped in frequency and are excluded from speed attribution.
These remain core benchmarks, below 60 fps, with no frontend presentation cost.

Six Windows regression tests pass; all six standalone H700 tests pass, with
the final modified-return case additionally checked on H700. Five fixed
Windows game windows still match prior CPU/video/PCM results. Android arm64
and armv7 rebuild and link with actual `-std=c23` (NDK Clang 19); the H700
build uses C23 (Clang 18). The older GCC 8 Windows build explicitly falls back
to C11. Android runtime and sustained Spruce gameplay/audio remain unverified.

Libretro now negotiates `GET_CAN_DUPE` before sending NULL video frames and
otherwise resends the last presented buffer, including video effects. The
isolated development core has not replaced the restored installed core.
Raw evidence: `leaf-wizard-ab.json`, `leaf-blue-ab.json`,
`counters-before-recycle.json`, `counters-final.json`, `resume-h700-tests.json`,
`resume-win-scenes.json` under `F:/GP32/results/`. Candidate artifacts are in
`F:/GP32/artifacts/c23-leaf-candidate-20261003/`.

The final H700 build also matched the prior hashes and CPU state for Her
Knights, Dungeon & Guarder and Astonishia (`resume-h700-scenes.json`). Their
single-run core rates are not interleaved optimization comparisons. The live
core and both the GP32 launcher and common RetroArch configuration hashes
were checked again after the isolated runs; no device setting was changed.

## Resume 2: AArch64 correctness and memory dispatch (2026-10-03)

The resumed tree contained unvalidated AArch64 block-transfer changes. Running
its existing regression on H700 reproduced 154 mismatches. Corrections cover
scaled load/store offsets, second-page physical mapping and pointer bias,
PC/base-register aliasing, and bounds/condition checks. MRS SPSR now uses the
checked path instead of reading CPSR, and MSR flags replaces the entire top
byte. The packed TLB mirror is read with paired loads; the RAM base is kept
in a callee-saved register for the native block, with aligned save/restore.
Indexed LCD scanout reuses the already-initialized 16-bit color lookup table.
All eight render modes retained identical framebuffer/repeat hashes.

Additional native tests exercise register-shift boundaries (including 0,
31/32/33, 255/256), carry flags, signed/unsigned long multiplication with
aliasing, PSR/MRC, and LDM/STM across independently mapped pages, PC/base
register lists, tiny pages and non-RAM fragments. Invalid newly drafted test
encodings/oracles were rejected and replaced; they are not counted as core
bugs. The corrected tests pass on x86-64 (29,299 JIT events, zero fallbacks)
and H700 (29,299 events, four fallbacks). The other five standalone H700
regressions also pass, including audio callback backpressure and persistence.

The strict H700 harness now requires a full ABBA, matching guest/video/PCM
results and at least two identical frequency samples per run, all at the
same observed frequency, before marking a performance comparison successful.
It records binary and harness hashes, exact commands and monitor failures.
For Wizard, baseline/candidate/candidate/baseline was
37.172 / 45.213 / 45.003 / 37.279 fps, all at observed 1512 MHz.
Median throughput improved 37.2255 -> 45.108 fps (+21.18%). Blue gameplay
candidate runs were 47.178 and 47.149 fps; the final baseline was 38.932 fps
at the same observed clock, but the first baseline ramped frequency. Its
full ABBA is explicitly disqualified; do not quote the raw aggregate ratio.
These are fixed core-only windows, still below 60 fps and excluding RetroArch.

Blue's measured helper operations dropped from 1,447,149 to 158,068.
Wizard remains dominated by single-memory helpers: 850,347 calls in its
120-frame profile, including 845,403 classified as non-RAM. Address-level
classification is still needed to separate BIOS from side-effecting MMIO
before designing another safe fast path. Cross-page block transfers are no
longer automatically classified as slow; both translations and RAM bounds
are checked by the diagnostic classifier, without modifying the TLB.

Five Windows fixed windows retain prior CPU/cycle/video/PCM results. H700
Her Knights, Dungeon & Guarder, Astonishia and Blue boot windows also match,
as do Wizard and the separate Blue gameplay state. These are bounded scenes,
not full-game completion or physical-GP32 accuracy evidence.

Raw evidence in F:/GP32/results/: resume2-jit-red.json,
resume2-jit-green.json, resume2-jit-final.json, resume2-h700-*.json,
resume2-wizard-abba.json, resume2-blue-abba.json, resume2-profile-blue.json,
resume2-profile-wizard.json, resume2-win-scenes.json, resume2-h700-scenes.json,
resume2-render-before.txt and resume2-render-after.txt. Bench SHA256:
664a221c483e8d17ab298b1b5879e6945eb23b48c71225a4eb3f735b09e8c332.

### Libretro memory serialization

The audit reproduced two independent state bugs: querying size wrote the
whole state to a temporary file, and serializing to a short buffer falsely
reported success after truncation. Libretro now uses a bounds-checked memory
stream shared with the existing file save/load wrappers. Size queries count
bytes without file I/O; memory save/load makes no temporary file. Short save
buffers reject before writing, and larger buffers receive zero padding.
The per-game reported capacity is stable; unexpected growth fails safely.

Windows and H700 focused tests cover real synthetic NAND roundtrip, file vs
memory byte equality, oversized buffer guards, truncated headers/bodies,
short save rejection, deterministic repeated serialization and the existing
audio/reset/load lifecycle. The untouched pre-change Windows file fixture
and post-change fixture have identical SHA256
2647394f74eea8f2dd245ae61ae28e1f9db9986326962123588002e12f900097.
No state-image struct layout or format version changed. Invalid loads retain
the existing partial-mutation semantics; this is not transactional loading.
The full NAND image remains in the payload, so full-speed rewind/run-ahead
is not proven merely by removing disk I/O.

Final H700 and Android ARM64/ARMv7 libretro cores rebuild in C23 mode; the
Windows GCC 8 validation build uses its explicit C11 fallback. The installed
Spruce core, GP32 config and common RetroArch config hashes remain unchanged
(resume2-final-snapshot.json). New development artifacts are kept locally in
F:/GP32/artifacts/c23-a64-state-candidate-20261003/, with source/binary hashes
in manifest.json. They have not replaced the installed core. Full-library
60 fps, sustained frontend audio/pacing and Android runtime remain open.

## Resume 3: GPIO attribution and dispatch (2026-10-03)

Address profiling now records all non-RAM SINGLE_DT counts by physical top
byte and the first 64 distinct addresses (with explicit overflow). It probes
only existing TLB mappings, makes no device read, and compiles out of release
execution. Both scene outputs still match the prior CPU/video/PCM results;
region sums and address-plus-overflow sums equal the parent counter.

Wizard's 120-frame profile has 845,403 non-RAM calls. GPIO accounts for
797,097 (94.3%), versus only 3,138 BIOS accesses. The dominant address is
0x15600024 (193,290 reads and 382,288 writes), followed by 0x1560000c
(193,363 reads and 2,939 writes). These are SmartMedia control/data accesses.
No address records overflowed in Wizard. Blue has 4,040 overflow accesses;
its address table must not be treated as a complete list or ranked sample.
Evidence: resume3-address-wizard.json, resume3-address-blue.json and
resume3-address-summary.json under F:/GP32/results/.

The GPIO write clause now precedes unrelated peripheral address comparisons.
Its body, masks and every gp32_smc_update call are unchanged; this does not
skip edges, latch updates or guest instructions. A synthetic NAND GPIO test
checks ID, program/readback, data-latch stability, read advancement and 16/32-bit
control writes. It passes against the pre-change Windows static library and
the changed Windows/H700 libraries. The existing LCD/timer test also passes.

The non-LTO Wizard ABBA is 45.301 / 45.682 / 45.549 / 45.186 fps, all at
observed 1512 MHz, with matching cycles, CPU state, pixels and PCM.
Medians 45.2435 -> 45.6155 (+0.82%) support only a small local gain.
Evidence: resume3-gpio-wizard-abba.json; candidate benchmark SHA256
 a2f4127b61eb9c520c2d2417c23781201750377400139b078549e8222f6cd35f.

An independent ThinLTO experiment on the pre-GPIO source measured Wizard
45.0955 -> 45.8995 (+1.78%), with the same checks and observed clock.
This is an alternative build experiment, not an additive gain measured on
the GPIO candidate. The local Zig wrapper toolchain could not locate the
IPO compiler archiver; the experiment used explicit -flto=thin compilation
and link flags with IPO disabled. Default build flags remain unchanged.
Evidence: resume3-lto-wizard-abba.json; experiment benchmark SHA256
 c5499da77d1796e991767e3a6e55db9dda72d87ed64d53d48723f0efb77fb88f.

ThinLTO Blue gameplay (600 warmup / 300 measured frames, a different window
from the 60/180 benchmark) produced 48.990 and 48.874 fps versus the final
matched-clock baseline of 47.171. The first baseline included 1416 MHz samples,
so the full ABBA is disqualified (resume3-lto-blue-abba.json). Do not quote
its aggregate ratio or claim a whole-library LTO win. Both runs retained
exact CPU/video/PCM output. LTO is still experimental and not the default.

The current non-LTO H700 core and both Android ABI cores rebuild. The GPIO
change is retained as a small measured optimization, with the above focused
regressions; core-only measurements do not prove frontend presentation/audio.
The candidate is F:/GP32/artifacts/c23-gpio-candidate-20261003/. Installed
core and global device settings were not changed during these /tmp runs.

## Resume 4: native instruction work (2026-10-03)

The SWE dispatch review suggested switching nonconverging polling candidates
back to native execution. Existing measured profiles do not justify making
that change now: Wizard has 197,321 portable-block instructions versus
107,417,887 native instructions, and Blue 111,223 versus 102,375,413.
Their polling skips save 10,884,792 and 41,513,364 instructions respectively.
Adaptive polling is deferred pending an actual workload with significant
nonconverging portable execution. This avoids weakening a useful fast path
on the strength of a hypothetical counterexample.

The AArch64 emitter now uses one AND-immediate instruction for encodable
masks, folds suitable ADD/SUB/CMP/CMN immediates, eliminates unused MOV/MVN
source-register loads, and omits x20/x21 save/restore for non-memory blocks.
No guest register-store elimination, chaining or flag liveness was added.
The 48-byte aligned frame and helper exit checks are preserved.

The pure logical-immediate encoder matched Zig/Clang assembly byte-for-byte
for all 1,302 distinct W-form masks; 4,098 additional inputs checked rejection
and unchanged output on failure (resume4-encoder-check.json). A new native
program records every immediate result and CPSR to RAM across zero, carry and
signed-overflow seeds, small/shifted12/large immediates, and flag/nonflag
arithmetic and moves. Windows passes; the old and changed H700 backends both
pass with 30,163 JIT events and four fallback events. Existing native edge
cases remain in that test. Raw runs: resume4-jit-before.json and
resume4-jit-after.json. Android ARM64 rebuilds with C23; the ARMv7 backend is
unchanged by this architecture-guarded include.

The first correct encoder searched candidate rotations. It improved Wizard
slightly but regressed Blue's matched-clock runs to 44.193/44.289 fps versus
47.160 for the final baseline. Blue compiles 67,529 blocks in its 180-frame
window; Wizard compiles 15,115 in the earlier 120-frame profile. Code generation
cost therefore matters. The parent replaced the rotation search with direct
circular-run transition detection and bit scans. The exhaustive assembler
comparison above was repeated for this final encoder and passed; generated
instructions are unchanged. The slow version was not retained as a candidate.

Final non-LTO benchmark SHA256:
 a8feebfc18635db2e173a7a30365145f0a9e2fe438a8e4fed803257685b0cc8b.
Against the resume-3 GPIO binary, matched-clock ABBA results are:

| Scene / warmup / measured frames | Baseline median | Candidate median | Change |
| --- | ---: | ---: | ---: |
| Wizard character selection / 2400 / 300 | 45.6335 fps | 47.0135 fps | +3.02% |
| Blue gameplay state / 600 / 300 | 46.7965 fps | 47.5210 fps | +1.55% |

Every run observed 1512 MHz, with matching cycles, CPU state, video and PCM.
Evidence: resume4-final-wizard-abba.json and resume4-fast-encoder-blue-abba.json.
The original Blue 60/180 window also retained exact output, but its first
baseline ramped from 1416 to 1512 MHz, disqualifying the full ABBA performance
ratio. Candidate runs were 47.627/47.435 fps versus the final baseline's 47.000;
these are a matched subset, not a qualified full comparison. Preserve that
negative qualification in resume4-final-blue-abba.json. The diagnostic Blue
profile also retains exact output (resume4-final-blue-profile.json).

A separate nonshipping SIGPROF sampler now starts after warmup and stops at
the measured-window boundary. It records PC plus instruction bits to avoid
mixing reused JIT addresses, resolves static symbols with their actual sizes,
and uses /proc/self/maps to distinguish generated code. Both sampled workloads
retained exact CPU/video/PCM output. Blue 60/180 collected 761 samples: 37.1%
generated code, 18.3% arm_jit_run, 7.5% gp32_get_framebuffer and 6.8%
s3c2400_read32. Wizard 2400/300 collected 636 samples: 55.3% generated code and
21.5% arm_jit_run. These are directional, signal-biased samples including
benchmark hashing; arm_jit_run includes inlined work and is not a pure
dispatch-cost measurement. The requested 1 ms interval produced about 100
samples/second on this kernel. Sampling FPS is not a performance result.
Evidence and reproducible sampler/analyzer: resume4-sample-{blue,wizard}.json,
resume4-sample-summary.json, resume4-bench-measured-sample.c and
resume4_analyze_samples.py in F:/GP32/results/.

The next justified targets are block execution/transition costs and Blue's
framebuffer reads. Internal block chaining still needs proof of budget,
interrupt, MMU and invalidation exits; no such chaining is enabled here.
Candidate artifacts: F:/GP32/artifacts/c23-native-immediates-candidate-20261003/.
H700 and Android ARM64 cores rebuild; unchanged ARMv7 is carried forward.
Installed core and global settings remain unchanged. All-title 60 fps,
sustained RetroArch audio/presentation and Android runtime remain unverified.

## Resume 5: framebuffer repair scan (2026-10-03)

Measured PC samples led to the existing Blue Angelo compatibility repair in
gp32_get_framebuffer. It scanned every pixel through repeated RAM-range and
aligned read32 callbacks. The scan now resolves the complete RAM surface once
per invocation and directly accesses bytes. The paired LUT/LCD guards,
left-to-right in-place neighbour propagation, eight-pass limit and isolated
yellow preservation are unchanged. No RAM pointer survives the call. For
custom RAM sizes ending partway through an aligned word, a callback fallback
retains the old final-byte read behavior; mapping requires whole-word coverage.

The focused gp32_shadow test uses synthetic LUTs/surfaces, not game assets.
It passes against the saved old source and new source on Windows, and the new
source on H700. Fixtures cover unpaired/paired LUTs, public framebuffer access,
connected versus isolated yellow, forward and reverse propagation including
the eight-pass limit, corner/row boundaries, non-indexed mode, invalid surfaces
and an incomplete final RAM word. The Command Code test agent did not deliver
a patch; the parent completed this verification. H700 raw result:
F:/GP32/results/resume5-shadow-h700.json.

Against resume-4, final matched-clock ABBA at 1512 MHz measured Blue's 600/300
window at 47.6305 -> 54.343 fps (+14.09%). Wizard 2400/300 was essentially flat
at 47.080 -> 47.055 fps (-0.05%). CPU state, cycles, video and PCM match in all
runs. Evidence: resume5-shadow-final-{blue,wizard}-abba.json. Benchmark SHA256:
 307387c4bf5839a4802f43ecc71a67e965c6174a0d824eb1ae1df91e6c424309.
The earlier draft without the custom-RAM fallback is retained only as an
experiment: its first Blue baseline changed clocks, so its aggregate speed
ratio is not qualified (resume5-shadow-blue-abba.json). H700 and both Android
ABI cores rebuild; installed device core/settings remain untouched.

The follow-up Blue 600/300 sampler retained exact output and collected 557
samples: generated code 42.2%, arm_jit_run 22.4%, gp32_get_framebuffer 1.3%.
The latter now contributes far fewer samples; the older profile used 60/180,
so this is not an exact percentage-point comparison. It supports continuing
with native execution/dispatch. Raw evidence: resume5-sample-blue.json and
resume5-sample-summary.json. Sampling remains diagnostic, not a speed benchmark.

Sol supplied one separate dispatch candidate combining the allocated block
header's valid/PC/CPSR/generation boolean checks. The null check remains
short-circuited. It encourages a single vector header load/reduction on
Cortex-A53 instead of repeated address calculations and branches. No budget,
IRQ/FIQ, polling or invalidation guard was removed. Windows and H700 JIT and
polling tests pass; the native H700 corpus reports 30,163 events and four
fallbacks. Independent matched-clock ABBA against the verified framebuffer
candidate measured Wizard 46.9575 -> 47.8455 fps (+1.89%) and Blue 54.092 ->
54.7045 fps (+1.13%), with exact CPU/video/PCM and 1512 MHz samples throughout.
Both changes are retained. Evidence: resume5-dispatch-{wizard,blue}-abba.json;
patch: resume5-dispatch.patch. Final benchmark SHA256:
 86a99d2b6e33fb75a37a6d8afc053a728e2ab5048a1c4324809cffd062f5506c.
The dispatcher change also rebuilds for H700 and both Android ABIs. Candidate
artifacts are F:/GP32/artifacts/c23-shadow-dispatch-candidate-20261003/.

Device testing deferred when RetroArch was detected, then resumed after the
user exited the game. No frontend process was killed. The read-only snapshot
resume5-device-snapshot.json confirms unchanged installed core, platform and
RetroArch configuration hashes. Its `pidof` exit 1 means no RetroArch process.
The tested fixed windows are still below 60 fps. Whole-game minima, sustained
frontend audio/presentation and Android runtime acceptance remain open.

## Resume 6: forwarding experiment and input scheduling (2026-10-03)

A narrow AArch64 experiment forwarded the preceding unconditional data result
in W2 into the next instruction's operands, retaining all guest/CPSR stores.
It invalidated across conditional/control-flow/helper/memory/PSR/regshift paths.
An adversarial native/interpreter fixture records results across those
boundaries, aliases, carry, shifts, SWP and mid-block entry. It passed before
and after the change on H700 (30,235 JIT events, four fallbacks), and its
pre-change Windows oracle also passed.

The change was rejected: matched-clock ABBA showed Wizard 47.834 -> 47.464 fps
(-0.77%) and Blue 54.579 -> 53.962 fps (-1.13%), despite exact CPU/video/PCM.
The emitter was restored byte-for-byte; rebuilt benchmark SHA256 again equals
the verified resume-5 binary 86a99d2b6e33fb75a37a6d8afc053a728e2ab5048a1c4324809cffd062f5506c.
The regression fixture stays because it tests architectural boundaries rather
than an implementation detail. Evidence: resume6-forward-{wizard,blue}-abba.json,
resume6-jit-{before,after}.json, and resume6-forwarding-rejected.patch in results/.

Input source review found polling and gp32_set_buttons occur before CPU work
in retro_run. The new libretro_input fixture goes further: a synthetic ARM
program reads the actual GPIO A-button bit into RAM, and the video callback
observes that guest-written value. Press and release reach it in the same
retro_run with JIT off and on, on Windows and H700. No core-added one-frame
input queue was observed in this fixture. This is not a physical controller-
to-display latency measurement or proof every game polls input immediately.
Raw H700 result: resume6-input-h700.json.

Audio recovery now attempts to drain pending PCM before the overflow path
discards old samples to admit a new block. The ordinary non-overflow path and
250 ms queue/allocation limit are unchanged. Previously, a frontend that had
fully recovered still lost the oldest retained block; partial recovery could
also discard samples that the callback was ready to accept. Tests cover both
cases with ordered stereo PCM, bounded storage, and no duplicate delivery.
The updated fixture failed four assertions against the pre-change source,
then passed on Windows and H700 after the fix. H700 test SHA256:
669bda2a1c5b68f475dbd5441706415560550f7811c5014fbc47ae152601037c.
Evidence: resume6-audio-before.txt, resume6-audio-after.txt and
resume6-audio-h700.json. This fixes avoidable loss during recovery, not a host
that persistently generates audio more slowly than real time. No sound-device
or RetroArch settings were changed. H700 and Android arm64/armv7 cores rebuild.
Packaged candidate: F:/GP32/artifacts/c23-audio-recovery-candidate-20261003/.
This candidate is not installed on the device.

## Resume 7: direct execution feasibility (2026-10-03)

A freestanding static AArch32 Linux EABI program ran successfully on the actual
Spruce H700 without OS changes. Its ordinary LDR of bytes 11 22 33 44 returned
0x44332211; an LDR from the next byte returned 0x55443322. The ARM920T legacy
unaligned-load rule, with alignment checking disabled, instead rotates the
aligned word to 0x11443322. This establishes both usable 32-bit execution and
a concrete semantic difference that a GP32 direct-execution backend must
handle. It does not establish whole-game compatibility or faster execution.
Probe source/result: F:/GP32/results/resume7-aarch32-probe.c and
resume7-aarch32-h700.json; binary SHA256:
f307e3467ae954e02310dbfd3f991636ef0af5df35ee6f42dcd94505aca4fc10.

An independent DeepSeek staging microbenchmark tested cache-tiled rotation
against the current libretro loop, including padded strides and alpha/channel
patterns. All output comparisons passed on H700. At the actual 240-pixel stride,
the baseline median was about 267 microseconds per frame and the best tiled
variants about 253-255 microseconds (roughly 5-6% for this loop only). The PC
probe regressed at this stride. Larger gains at synthetic stride 256 do not
describe the current core layout. No production change was made: the measured
absolute saving is small, and this microbenchmark neither monitors matched
CPU clocks nor measures whole-game speed. Evidence: resume7-video-stage.c and
resume7-video-stage-h700.json in results/.

The parent added a separate diagnostic harness exercising real retro_run,
rotation, resampling and consuming video/audio callbacks. Its first Blue
600/300 run completed with deterministic output and 51.599 fps, but that single
unpaired observation is not a performance comparison with the core harness or
actual RetroArch. It does not include real display/audio drivers. Sources and
result: resume7-libretro-bench.c, resume7-libretro-baseline-blue.json.

The Samsung S3C2400 manual, chapter 10 (printed pp.10-1,10-3,10-7), confirms
that TCMPB controls the PWM output transition while the counter determines
reload and interrupt timing. The old cnt-cmp+1 period formula incorrectly
shortened the IRQ period, even though MAME uses a similar expression.
The core now excludes TCMPB from that calculation. This narrow correction
retains the existing cnt+1/zero-count convention; it does not claim complete
cycle-accurate PWM or implement the missing count-buffer latch semantics.
A new test runs all five timers at the same count/divider with differing duty
values and changes a duty buffer mid-period, checking coincident countdown
and repeated IRQ events against timer 4 (which has no compare register).
It failed the old core at cycle 2 and passes on Windows and H700 after the
correction. Evidence: resume7-pwm-before.txt, resume7-pwm-after.txt,
resume7-pwm-h700.json. The real-game checks and JIT optimization measurements
are kept separate so a timing change cannot masquerade as a speedup.
The corrected SoC linked with the frozen CPU library retains the existing
Wizard 2400/300 and Blue 600/300 CPU/video/PCM results exactly. These are
compatibility checks, not a performance comparison: resume7-pwm-{wizard,blue}.json.

A fresh Windows x86-64 libretro build also uses C23 (Zig 0.13/Clang 18.1.6,
GP32EMU_REQUIRE_C23=ON), rather than the existing GCC 8/C11 fallback build.
The produced DLL exports the libretro entry points. Its audio recovery,
x86-64 JIT, LCD timing and PWM regression executables all pass locally.
This verifies the C23 Windows core and focused behavior, not the standalone
Windows UI or an installed RetroArch session. The build emits a Windows API
version macro-redefinition warning between Zig's default and the project's
existing minimum; no source/API minimum was changed for this build.

Sol's next AArch64 experiment kept guest registers in fixed W0-W14 mappings
inside contiguous, unconditional, flagless, non-PC, non-shift ALU regions.
It flushed every written register before observable boundaries, and its new
SWI/exception-banking fixture passed on Windows and H700 (30,319 JIT events,
four fallbacks). The stable-poll test passed too. A synthetic 17-ALU region
used 15 instead of 36 guest loads/stores, but that is not game-wide evidence.
The candidate was rejected: matched-clock Wizard ABBA was 46.982 -> 47.145 fps
(+0.35%, insufficient gain). Blue's complete ABBA was disqualified because its
first baseline ramped from 1416 to 1512 MHz; the steady candidate runs were
53.597/53.548 versus the final steady baseline 54.474, offering no reason to
retain the extra compiler complexity. No qualified Blue percentage is claimed.
The emitter is restored byte-for-byte and the architectural regression stays.
Evidence: resume7-jit-{wizard,blue}-abba.json, resume7-jit-h700.json,
resume7-poll-h700.json, resume7-alu-rejected.patch. These experiments used the
same frozen SoC and separately compiled baseline/candidate CPU objects.

DeepSeek's real-retro_run timing harness retains baseline CPU/video/PCM output.
In its Blue 600/300 diagnostic, CPU execution plus SoC consumed 86.07% of
process CPU time; framebuffer fetch 2.56%; the harness's video copy/hash 8.19%;
audio callback hash 0.07%; remaining frontend work including rotation and
resampling 3.10%. Total process CPU p50/p95/p99 was 19.647/21.451/22.240 ms.
These times include instrumentation and synthetic callback hashing, not actual
RetroArch driver work or physical input/display latency. They prioritize CPU
execution work without assuming every executed guest instruction draws pixels.
Evidence: resume7-libretro-timing.c, resume7-libretro-timing-blue.json.

The existing resume4-final-blue-profile.json also records 67,529 compilations,
6,806 block conflicts and 90 CP15 cache invalidations in 180 measured frames,
with zero arena-recycle invalidations and a 1,831,840-byte high-water mark.
That evidence makes cache-maintenance/retranslation a concrete next target;
simply allocating more than the current 64 MiB code arena does not address it.
Any reuse must still detect modified guest instructions and preserve exception,
MMU and cache-maintenance semantics. No invalidation guard was removed.

Final audio/PWM candidate builds for H700, Android arm64/armv7 and Windows x64
are packaged under F:/GP32/artifacts/c23-audio-pwm-candidate-20261003/ with
source/artifact hashes. They are development candidates, not installed cores.


## Resume 8: instruction-cache maintenance attribution (2026-10-03)

An external diagnostic CPU copy counts CP15 writes only after the benchmark
warmup boundary. Blue's 60/180 saved-state window issues c7,c5,0 exactly 90
times at PC 0x0c7b14fc; Wizard's 2400/300 window issues it 47 times at the same
routine. The accompanying c7,c14,2 D-cache clean/invalidate-by-index counts are
46,080 and 24,064. Both diagnostic runs preserve the seven CPU/video/PCM
comparison fields. Diagnostic throughput is not a qualified performance result.
Evidence: F:/GP32/results/resume8-cp15-{blue,wizard,exact}.json and resume8diag/.

ARM920T TRM DDI 0151C Table 2-16 (printed page 2-18) defines c7,c5,0 as full
instruction-cache invalidation. This is real maintenance, not an ignorable
prefetch flush. The optimization candidate therefore verifies source instruction
bytes before retaining translations. Full MMU/state/API/recycle invalidations
must remain distinct; unchanged guest bytes do not make an overwritten native
code arena or stale mapping safe.


Selective CP15 cache revalidation retains generation/arena storage and verifies
all recorded instruction addresses using the current TLB hit or MMU-off mapping
and side-effect-free fastmem. Changed/unprovable blocks lose valid status;
MMU, API, reset/state and arena-recycle events retain full invalidation. MCR
still terminates every trace. There is no new check on the hot dispatch path.

Blue, saved state, warmup 1200 / measured 300, matched 1.512 GHz ABBA:
54.553 -> 61.297 core benchmark fps (+12.36%). CPU/video/PCM fields match.
This is fixed-scene uncapped core throughput, not a sustained RetroArch minimum.
The prior warmup-600 ABBA is retained but disqualified because its first baseline
included a 1.416 -> 1.512 GHz transition. Evidence:
resume8-cache-blue-warm-abba.json (qualified), resume8-cache-blue-abba.json
(disqualified). Neither governor nor frontend settings were changed.

Windows x64 Blue 600/3000 ABBA also preserves all exactness fields and shows
668.0205 -> 873.5945 core fps (+30.77%). Host clock/background-load qualification
is absent and candidate samples vary; treat this as a local directional result,
not an expected percentage on other PCs. Her Knights, Dungeon & Guarder and
Astonishia 2400/300 CPU/video/PCM windows remain exact on Windows.
Evidence: resume8-win-blue-abba.json, resume8-scenes-win-exact.json.


Wizard's matched-clock 2400/300 ABBA remains essentially unchanged:
47.7595 -> 47.757 core fps (-0.005%, within run variation), all exactness fields
match. The cache-reuse gain is title/workload dependent. Evidence:
resume8-cache-wizard-abba.json. H700 native differential, arena-churn/generation
wrap and stable-poll checks pass in resume8-{jit,recycle,poll}-h700.json;
Windows C23 arm_jit and arm_jit_recycle also pass. Android arm64/armv7 and
Windows x64/H700 libretro builds succeed. Android runtime is not tested.


The first scan-all-slots candidate above is not the final implementation:
Dungeon 2400/1200 exposed a qualified regression, 209.459 -> 185.711 fps
(-11.34%). Evidence: resume8-cache-dungeon-abba.json. The follow-up tracks slots
used in the current generation and scans that bounded list rather than all
16,384 sparse block headers. A slot is inserted only on its first translation
in a generation, so conflicts and partial invalidations cannot duplicate it;
full invalidation clears the list before generation wrap/reuse. This adds
64 KiB plus one count to transient CPU metadata, not to save-state data.

The profiling comparison is for the initial scan-all candidate: Blue 60/180
compiles 67,529 -> 10,398 blocks (-84.60%) while c7 I-cache maintenance stays
90, native instructions stay 102,375,413, and native calls stay 8,741,060.
Retaining native code raises observed arena usage from 1,386,700 to 38,112,508
bytes; the existing 64 MiB cap and full recycle mechanism remain. Evidence:
resume8-{before-blue-profile,after-blue-profile,profile-comparison}.json.

Standalone Windows WASAPI recovery no longer mutates the resampler on the pump
thread while submit may process it. The existing locked underrun flag delivers
the reset to its producer thread. The Windows-only win64_audio CMake test drives
real pump/submit functions with a fake endpoint and confirms ownership and gap
recovery; it passes in C23 without opening audio devices. Endpoint buffering
policy remains unchanged. Speaker quality, physical latency and standalone UI
runtime are not proven by this test.


The active-slot scan also regressed Dungeon: 209.5415 -> 197.643 fps (-5.68%)
under qualified matched clocks. It is discarded along with its 64 KiB list.
The current candidate uses a separate cache-maintenance epoch in the existing
block header tag. A guest MCR advances that epoch without touching arena bytes;
the next dispatch of an older block verifies every recorded source instruction,
then refreshes its epoch only on exact match. Changed/unprovable blocks translate
normally. This preserves the former retranslation-on-next-use timing and avoids
work for dormant blocks. MMU/state/API/recycle events still change native arena
generation and reset the cache epoch. Both counters' wrap paths are tested.
Evidence for the rejected scan: resume8-active-dungeon-abba.json. The final lazy
candidate uses a distinct binary/evidence series, resume8-lazy-*.


Lazy candidate source review found no new lifetime/epoch/mapping blocker. Its
Windows and H700 JIT differential and arena/epoch-wrap tests pass, and all four
C23 libretro targets build. H700 benchmark SHA256:
491015a7631f892c4b3c70b0f669cc7c45c9576ab9fd1d40239e333885cbf4d4.
The final qualified measurements below supersede the discarded scan variants.

- Blue 1200/300: 54.7035 -> 66.806 fps (+22.12%).
- Dungeon 2400/1200: 209.611 -> 237.6295 fps (+13.37%).

Both ABBA series have all measured frequency samples at 1.512 GHz and identical
CPU/video/PCM fields: resume8-lazy-{blue,dungeon}-abba.json. These are fixed-scene
core-throughput comparisons, not actual frontend frame-pacing or whole-game
minimums. Windows Blue 600/3000 is 695.418 -> 945.057 fps (+35.90%) with identical
outputs but no clock qualification: resume8-lazy-win-blue-abba.json. Final
Windows Her/Dungeon/Astonishia windows are also exact in
resume8-lazy-scenes-win-exact.json. The lazy candidate replaces the extra
active-slot list with one transient epoch; no save format change is needed.


Final Wizard 2400/300, qualified 1.512 GHz ABBA, also improves:
47.7405 -> 50.493 fps (+5.77%), all exactness fields match. Evidence:
resume8-lazy-wizard-abba.json. It remains below 60 core fps in this workload;
no all-game 60 fps or real-time audio/presentation guarantee follows from these
results. The source and tested core binaries retain the lazy candidate.


Final lazy Blue 60/180 profiling confirms 67,529 ->
8,041 compiled blocks, with the same 90 cache operations and
identical CPU/video/PCM and native instruction/call counts. Its arena occupancy
is 29,079,868 bytes in that window, within the unchanged 64 MiB
cap. Evidence: resume8-lazy-profile-comparison.json. Final H700 Her Knights and
Astonishia 2400/300 hashes also match the pre-change Windows reference; these
single runs are compatibility evidence, not qualified speedup measurements.
Final stable-poll regression passes in resume8-lazy-poll-h700.json.

Device readback in resume8-final-device-snapshot.json confirms the installed
core, GP32 launcher configuration and common RetroArch configuration retain
their prior hashes. No RetroArch process is running. Development binaries are
packaged separately in F:/GP32/artifacts/c23-jit-cache-candidate-20261003/;
the standalone WASAPI source fix is not a packaged standalone UI executable.

### Korean Her Knights combat benchmark (resume9, 2026-10-03)

The user's `Her Knights - All for Princess - Deadline (Korea).smc` matches
MAME's Korean **그녀의 기사단 강행돌파** entry by size and SHA-1. Earlier Her
results used Europe/boot-menu windows. A separate first-palace combat state and
scripted movement/attacks now provide a visually checked gameplay workload.
Qualified H700 1.512-GHz ABBA measures 81.986 -> 90.5365 core fps (+10.43%) for
the retained lazy cache candidate, with all seven CPU/video/PCM fields exact.

The same 20-emulated-second window produces PCM on all 1200 frames, with zero
empty/audio-error frames, 220631 stereo samples at reported 11035 Hz, and
19.993747 seconds of source audio. Only 305 distinct framebuffer transitions
are observed (15.25 per emulated second), despite sufficient core throughput.
This separates low game screen-update frequency from missing core PCM in this
scene; it does not prove original hardware timing or absence of audible artifacts.

Actual RetroArch reached a 60.06-fps final OSD in an idle combat state but ALSA
failed EBUSY because MainUI retained the PCM device. That run continued without
audio and MUST NOT be counted as sound validation. No UI process was stopped.
Preparation also found the 256-MiB temporary filesystem full of accumulated
development files; this turn's ROM/state assets were relocated to the SD
developer folder and the partial temporary upload removed. No causality for the
earlier OS incident is established. Details and raw evidence pointers are in
[the Korean combat report](HER_KNIGHTS_KOREA_BENCHMARK.md).

### Paired AArch64 transfers and staging (resume10, 2026-10-03)

A bounded candidate paired adjacent registers in single-page, ordinary LDM/STM
transfers using W-form LDP/STP for RAM and the guest register array. Existing
alignment, mapping, bounds, PC, writeback and two-page handling stayed intact.
The H700 native differential suite, including eight representative contiguous,
gapped, odd-tail and base-overlap cases, passed. However matched 1.512-GHz ABBA
showed no useful end-to-end gain: Wizard 2400/600 50.6645 -> 50.706 fps (+0.08%),
Blue state 1200/600 66.7215 -> 66.5205 fps (-0.30%). Both were output-exact.
The candidate and its new tests were archived outside the checkout and removed
from the production sources. Rebuilding restored benchmark SHA-256
491015a7631f892c4b3c70b0f669cc7c45c9576ab9fd1d40239e333885cbf4d4 exactly.
Evidence: F:/GP32/results/resume10-pair-{jit-h700,wizard-abba,blue-abba}.json;
archived source: resume10-rejected-pair-arm920t_jit_a64.inc and
resume10-rejected-pair-test.c in that directory. A new local capture identifies
the Wizard 2700-frame point as character selection, not an established battle
workload (resume10-wizard2700.png).

The retained benchmark tooling change stages candidates under ROOT/bench-bin
by default, with an explicit --remote-dir override, rather than accumulating
them in the device's small /tmp. Exact existing hashes are reused; a new file is
uploaded to a unique sibling, hash-checked, marked executable and renamed before
use. An existing content-addressed file with a different hash is preserved and
rejected. The RetroArch guard still precedes mutations. The two real ABBA runs
above verified initial SD staging and subsequent reuse respectively, recorded
as candidate.reused=false/true. Measurement/exactness/clock logic is unchanged.

The Korean battle exposed a real cross-host input-replay bug. Its input script
contained two frame-zero SETs, RIGHT then RIGHT+A. Sorting by frame/action alone
allowed qsort to reverse these equal keys on Windows while retaining their order
on H700. A trace-interpreter state probe identified the first difference as a
single SoC.buttons byte (0x200 versus 0x201); CPU, RAM, timer, audio and all other
state bytes were still identical. The later video/PCM discrepancy was caused by
different input, not by a demonstrated JIT or timer defect.

Input events now carry their insertion sequence as a final comparison key.
Existing frame/action precedence remains; equal-frame/action events retain
authored order. A focused replay fixture fails before the change on Windows and
passes after it on Windows and H700. Replaying the original Korean combat script
for 1200 warmup + 1200 measured frames now matches all seven CPU/video/PCM fields
across both hosts and the prior H700 candidate. H700's single corrected run is
90.236 fps, not a qualified performance-improvement claim. Evidence:
F:/GP32/results/resume10-input-{test-h700,fixed-win,fixed-h700,cross-host-exact}.json;
diagnosis: resume10-portability-findings.md. The rebuilt Windows headless runner
also contains the fix. This affects recorded/scripted input order, not physical
controller latency. Actual speaker output and input-to-display latency remain
open requirements.

## Resume11: normal Spruce audio path and real battle scene capture

The Korean Her Knights candidate now passes normal Spruce app launch, ALSA
PLAYBACK initialization and automatic return to MainUI. The prior direct-SSH
audio failure was menu ownership of the PCM device. A temporary developer app
uses the normal exit/respawn flow; no menu process kill or installed core/settings
replacement is required. In the 1800-frame idle battle run, the screenshot
shows about 60 fps, with 1784 threaded video frames pushed / 17 dropped. ALSA
buffer is 3072 frames with four 768-frame periods. Twenty-nine sampled RUNNING
states have one unchanged trigger time, but sparse status sampling is not proof
of no glitches. RA's 4.22% OSD underrun is a software-FIFO near-empty statistic,
not actual XRUN or silence-insertion count. Details and evidence are in
HER_KNIGHTS_KOREA_BENCHMARK.md and F:/GP32/results/resume11-audio-metric.md.

Final ALSA counter evidence from interposed probes is in
HER_KNIGHTS_KOREA_BENCHMARK.md: writei 1985 (timeline) and 1982 (combat), zero
wait/recover/prepare/start calls, zero EPIPE/ESTRPIPE/EAGAIN/other errors and
zero recovery calls in both runs. Zero-frame counters concentrate at startup
and trailing (all-zero 86,352 / 84,689; trailing 86,019 / 84,371; longest run
768 = one ALSA period) and the 29 steady middle seconds hold 330 zero stereo
frames, 5-21 per second, which cannot be distinguished from the game's own
silent PCM. The run was an idle battle window (auto-state, no scripted attack)
and no physical speaker recording or listening exists. Evidence:
F:/GP32/results/resume11-final-evidence.json.

CommandCode's separate local scene exploration reached actual Little Wizard
combat. F:/GP32/results/resume11-wizard-combat-entry.state (frame 3350) and
resume11-wizard-combat-x3900.state have corresponding inspected combat captures;
resume11-wizard-combat.txt reproduces entry from boot. The old 2700-frame
benchmark was character selection. Real-combat H700 throughput is still to be
measured. Repeated identical intermediate captures were a headless dump-at
scheduling defect in frame mode, not evidence of an emulation hang.

## Resume12: descriptive status of the current optimization candidates

The integrated CPU, LCD, audio and presentation changes build on Windows,
H700 Linux, and Android ARM64/ARMv7. The existing Windows CTest suite passes
12/12. This does not establish all-game 60-fps operation or physical audio quality.

**AArch64 JIT RAM-page handling.** src/arm920t.c, src/arm920t_jit_a64.inc,
tests/arm_jit_test.c. Windows differential: baseline and candidate both PASS
with the expanded test (36,110 JIT events, 0 fallbacks each; the original
baseline test had 30,524 events). H700 cross-build PASS (Zig 0.13.0,
aarch64-linux-gnu.2.17, cortex_a53, C23, O3; test binary SHA-256
237eaa247c0fe2e4405fa45f32d5b0efb7bc7638cce28ce5d9a16ca0df428629). The native
H700 full gate FAILED on mapped-page-RAM-end (CP15[5] = 5 vs 0,
CP15[6] = 0c7fffff vs 0). Root cause was a pre-existing classified halfword
physical fallback that translated an already-physical address again and mutated
CP15 fault registers; the repair probes physical fastmem and calls the physical
bus callbacks directly. Bounded baseline bytecode fails the same two CP15
mismatches while bounded fixed bytecode passes (events = 30, fallbacks = 0).
The repaired native H700 RAM-end case and full differential gate now pass
(36,108 JIT events, 4 fallbacks in the full run). Evidence:
`resume12-jit-final-h700.json`, binary SHA-256
`f5b79f4e2de20b7a3a3c7f4a3db859caf19fbc47d55020890c488aae139b93f1`.
The candidate adds 32 KiB transient A64 metadata plus derived TLB fill/flush work.

The integrated resume12 H700 battle comparison is exact across CPU, video and
PCM: Her Knights Korea improves from 90.098 to 101.2165 core fps (+12.34%) in
ABBA order at matched 1.512 GHz. This combines CPU/LCD changes; it does not
isolate the JIT cache's contribution or measure the whole libretro audio path.
Details: `HER_KNIGHTS_KOREA_BENCHMARK.md`.

**LCD contiguous scanout fast path.** src/s3c2400.c. It engages only when
OFFSIZE == 0, the width is aligned to pixels-per-word, and the whole consumed
word run is inside RAM; every other case keeps the untouched per-halfword path.
Host equivalence harness: 468 cases, 0 failed, colour LUT 0 mismatches over
65,536 entries, plus 400 fixed-seed random cases. Host proxy timings (x86_64,
mingw gcc 8.1.0 -O2, not H700): about 2.5-3.7x for the common 16-bpp 240x320
PAGEWIDTH=240 OFFSIZE=0 format and about 1.8-2.6x for aligned 8/4/2-bpp; the
fallback cases are the same source and their 0.7-1.2x swings are host noise. The
integrated candidate builds through the project's CMake/Zig path. A title with
OFFSIZE != 0 does not take this fast path.

**Audio resampler.** src/audio/gp32_audio_resampler.c. Harness bench with
alternating A/B order, ns per produced output frame, 13 cases over
11035/22050/44100/48000 Hz pairs with and without fade. Reported speedups are
best-case 1.20-1.44x and median 1.23-2.05x; the 11,035 to 11,035 case has a
noisy reference median (2.105 ns per frame), so its 2.05x median is not
comparable with the other cases. These are host harness numbers, not device
results.

**Boot/menu baseline for the local library.** resume12-all-games-bench.json
(Windows, 2400 warmup + 300 frames, --jit, autopulse input) produced metrics for
all 28 unique owned ROMs with 0 errors and 0 timeouts. Every row is a
boot/menu/attract window, not gameplay; the table and dedup method are in
docs/GP32_LOCAL_GAME_MATRIX.md.

**Presentation and optional effects.** The libretro portrait rotation uses
8 x 8 tiles. Its H700 harness matches pixels for five dimension/stride cases;
average staging cost is 0.628 ms before versus 0.572 ms after (about 9% less
time). This is a component measurement, not an overall game speedup.
Packed RGB arithmetic and specialized loops speed up the optional persistence
and interpolation effects; their validation is host-side.

**Additional correctness fixes.** FXP3 RAM-range checks now reject addresses
above RAM before subtraction can wrap. A malformed payload previously caused
an out-of-bounds read and now fails normally. SDL 1.2/SDL3 latency trims use a
short ramp at the splice to reduce discontinuities in dropped audio; synthetic
device-clock probes verified unchanged ordinary playback and smaller trim
steps. Physical SDL audio and starvation-free operation remain unverified.

**Packaging and library.** The Android helper builds both ABIs with NDK
28.2.13676358 and C23, and emits a SHA-256 manifest. Runtime testing on Android
remains open. Spruce's GP32 tile is 120 x 130 pixels and was visually checked
after MainUI reloaded its texture cache. Twenty unique locally supplied Korean
releases were installed and hash-verified; the two previous European releases
were preserved separately. No ROM or BIOS is included in this repository.

**Installed H700 build.** The integrated libretro core ran through the normal
Spruce app exit/respawn path for 1800 frontend frames. ALSA PLAYBACK initialized,
the idle battle capture showed 60.27 fps, and RetroArch exited successfully
after 29 seconds of content runtime (1782 video frames pushed, 19 dropped).
This run had no ALSA interposer or physical audio recording and is not a
worst-case combat benchmark. The verified core was then installed at
`Emu/GP32/gp32emu_libretro.so`, SHA-256
`b30219f7114bc5e43ae82466289c813c16557d0757ae9c0329aceaa615836a58`.
The previous core and temporary diagnostic app remain in the device's
`gp32-dev` backup directory. GP32 configuration and common RetroArch settings
match the pre-promotion hashes. Local evidence: `resume12-final-ra.log`,
`resume12-final-ra.png`, `resume12-core-installed.json`.

## resume13: audio continuity and Korean gameplay coverage

Relative to `006fab9`, IIS DMA batches reserve capture space once and append
PCM with local FIFO cursors. Growth checks use `SIZE_MAX`, including on
32-bit Android, and zero-frame batches avoid arithmetic on a null buffer.
If bulk allocation fails, per-frame allocation can still retain part of the
audio; allocation-failure outcomes are best effort, not byte-exact guarantees.
The final host comparison matched all PCM bytes and 52 serialized-state samples
across 14 cases; all 10 expected normal PCM-producing cases actually emitted
audio. Three small streams also matched independently generated sample order
and FIFO carry expectations. Evidence: `resume13-iis/iis-parent-final.json`.

Two H700 A/B/B/A comparisons used dynamic guest clocks, 1200 warmup and 1200
measured frames with scripted gameplay. All seven CPU/video/PCM fields matched
in every run. Observed measurement clocks were consistently 1.512 GHz (44 and
46 samples respectively); no governor or audio setting was changed.

| Korean gameplay scene | 006fab9 median core fps | IIS batch median core fps |
| --- | ---: | ---: |
| Her Knights, first palace battle | 101.025 | 101.1425 |
| Tomak, stage-1 shooting with movement and bullets | 102.472 | 102.371 |

These differences are within run variation: no overall game speedup is
established for this change. These are uncapped core throughput figures, not
displayed frame rates or guarantees about later stages. Local evidence:
`resume13-iis-her-abba.json`, `resume13-iis-tomak-abba.json`. Candidate bench
SHA-256: `6f7321815b278bd4b054ab4691ffc245cc5792fe65e0b5da5636e95f7043ce2e`.

SDL 1.2 now fades a partially filled callback into silence and fades resumed
audio back in; SDL3 fades resumed chunks after an empty queue. Ordinary playback
remained byte-identical in virtual-clock probes. With a deliberately starved
SDL 1.2 source, large sample steps fell from 81 to 0 in that probe. SDL3 removed
the 42 restart edges, but cannot repair the 43 edges before a gap after SDL has
already consumed the queued data. Real SDL 1.2/SDL3 header compilation passed
with GCC and Zig; physical listening remains unverified. These backend changes
are separate from RetroArch's audio output path.

The headless runner can now write a native state with `--save-state` and returns
a failure status when the write fails. Korean Little Wizard combat, Tomak
shooting and Mill's input-responsive first room now have repeatable states and
scripts; see `GP32_LOCAL_GAME_MATRIX.md`. States and ROMs remain external.
The libretro input regression also checks that loading a state cannot keep a
stale button pressed after the frontend has released it.

Validation: the existing full Windows suite passed 12/12 during integration;
after the final IIS memory-bound changes, the six affected peripheral and
libretro audio/input/persistence cases passed. Android ARM64 and ARMv7 core
builds passed again. No Android runtime or physical input-latency measurement
was performed. The installed Spruce core remains the previously validated
resume12 build; resume13 binaries are development candidates.

## resume14: compact JIT dispatch and LCD loop specialization

The JIT table now stores compact headers separately from decoded instructions.
On 64-bit hosts a header is 32 bytes instead of a 2336-byte combined slot:
the 16,384-entry hot table is 512 KiB rather than 36.5 MiB. Total allocation is
essentially unchanged. Decoded instruction addresses remain stable for native
helpers; state serialization, generation checks and cache-maintenance semantics
are unchanged. Allocation failure frees the header table before falling back.

With this change alone, matched-clock H700 A/B/B/A comparisons against `2a9f64b`
produced the following core throughput. Both comparisons matched all seven
CPU/video/PCM fields and all measured frequency samples were 1.512 GHz:

| Korean gameplay scene | Baseline | Compact headers | Change |
| --- | ---: | ---: | ---: |
| Her Knights, 1200 warmup / 1200 measured frames | 101.3905 | 103.6500 | +2.23% |
| Little Wizard, 1800 warmup / 600 measured frames | 94.8465 | 97.2635 | +2.55% |

The Wizard comparison followed the Her Knights run without an idle interval.
Its earlier cold-device comparison is excluded because frequency was still
ramping. These windows differ from earlier Wizard runs and are not game-wide
minimum frame rates. Evidence: `resume14-compact-her-abba.json` and
`resume14-compact-wizard-warm-abba.json`.

The final combined candidate also matched Her Knights CPU/video/PCM output.
Its first throughput comparison overlapped a diagnostic search and is excluded;
the subsequent quiet comparison had a frequency ramp in its first baseline.
Therefore neither establishes an additional combined speedup percentage. The
two steady candidate runs in the quiet comparison were 103.674 and 103.278
core fps; use the qualified table above only for the isolated header change.
Evidence: `resume14-integrated-her-abba.qualification.md` and
`resume14-integrated-her-quiet-abba.json`.

The integrated candidate also replaces AArch64 flag mask/OR sequences with a
low-field BFXIL merge: new upper flags remain in the destination register and
old CPSR low bits are inserted at bit zero. Arithmetic, logical, multiply and
MSR paths preserve their different flag masks. A proposed nonzero-lsb splice
was rejected during review and was never applied to the repository.

For contiguous 16-bpp LCD output, four specialized loops select the fixed byte
order once per frame. Odd widths, OFFSIZE gaps and unbacked RAM keep their
existing paths. H700 execution of the independent LCD comparison passed all
468 cases and all 65,536 color-table entries. Host-only component measurements
showed roughly 19-24% less time for the two full-frame 16-bpp cases; that is not
an H700 or whole-game speedup claim.

Integrated validation: H700 native differential passed 36,108 events with four
expected fallbacks, Windows CTest passed 12/12, and Android ARM64/ARMv7 builds
passed. The libretro audio callback investigation found no reproduced ordering
or loss defect when the frontend could accept data. Partial/blocked callbacks
and recovery passed a separate probe; no production audio-buffer policy was
changed. Actual speaker quality and physical input latency remain open.

Profiling correction: `jit_block_conflicts` now counts only eviction of a
different PC, excluding retranslation of modified code at the same PC. Wizard's
corrected count was 7,070 versus the former mixed count of 11,422. A simple
high-address XOR hash increased true collisions to 7,947 and translations from
11,814 to 12,838; it was rejected. Keep the direct-map index unchanged.

The integrated core then ran through Spruce's normal command handoff and menu
return for 1800 frontend frames. The capture shows the expected Her Knights
battle and 60.29 fps; ALSA initialized, RetroArch exited with status zero and
reported 1784 video frames pushed / 17 dropped. This idle-scene run is not a
worst-case combat or physical audio acceptance test (the debug overlay still
reports audio underruns). GP32 and shared RetroArch configuration hashes were
unchanged. After review, the tested core was installed as
`Emu/GP32/gp32emu_libretro.so`, SHA-256
`e99a75dade30c3804c1bff2c7e620f095404082257aa485e46cf8ec756e9a085`.
The previous installed core is preserved at
`gp32-dev/resume14-installed-core-before.so`; the temporary command was consumed
and no diagnostic app was added to the menu. Local records:
`resume14-runtime.log`, `resume14-runtime.png`, `resume14-core-installed.json`.

## resume15: Korean library and active-combat audio delivery

The live Spruce menu shows 20 Korean-region releases and the GP32 system tile
at the same scale as its neighbors. The existing 120 x 130 icon matches the
repository hash; no additional image resize or device setting change was needed.
Six catalogued Korean releases remain absent from the supplied assets, as listed
in the [packaging guide](../packaging/spruce/README.md). Region filenames alone
do not verify every game's displayed language or full compatibility.

The inventory scanner now treats duplicate ZIP member names as ambiguous,
including when the two entries contain different ROM bytes. Previously it could
silently select the first entry. One-ROM archives retain their existing behavior.

### Tomak Korean combat: core-to-frontend and ALSA counters

A diagnostic-only wrapper of the installed `42876cf` core ran the existing
Tomak Korean stage-one state through the normal Spruce/RetroArch launch path.
It replayed the same 2,400-frame firing and vertical movement pattern as the
resume13 headless scene. The wrapper counted audio batch acceptance; the
previous ALSA interposer counted actual libasound errors and recovery calls.
Neither diagnostic library replaced the installed core.

| Observation | Result |
| --- | --- |
| Core runs / batch calls | 2,400 / 2,400 |
| Offered / accepted stereo frames at the libretro boundary | 1,763,352 / 1,763,352 |
| Partial / zero callback returns | 0 / 0 |
| Pending core output after each run | 0 frames |
| ALSA write calls / accepted output frames | 2,621 / 2,012,928 |
| ALSA errors / recovery calls | 0 / 0 |
| Threaded video frames pushed / dropped | 2,386 / 15 |
| Mid-combat / final OSD fps | 59.87 / 60.45 |

Screenshots show active bullets, enemies and damage; these are presentation
readings in one scene, not whole-game minimum rates. The final OSD's 42.69%
"underrun" value is still the software FIFO near-empty statistic described
above, not an audible-dropout percentage. ALSA zero-frame counts include normal
game silence and startup/shutdown padding, so these counters cannot replace
physical listening. GP32 and common RetroArch configuration hashes are unchanged,
and MainUI returned normally after exit 0. Evidence under `F:/GP32/results/`:
`resume15-runtime-verified.json`, `resume15-delivery.json`, `resume15-alsa.json`,
`resume11-ui-resume15-combat.png`, and `resume15-runtime.png`.

### Input polling

Libretro now negotiates optional joypad bitmask support and reads the pad in
one callback instead of ten when available. Unsupported frontends retain the
individual-button path. Poll-before-run order, GP32 button mapping and state-load
input behavior are unchanged. This reduces callback overhead; it is not a
measured controller-to-display latency reduction or whole-game speedup.

The focused input, audio and persistence tests pass in the C23 Windows build.
The input test also passes natively on H700 with the interpreter and JIT, including
bitmask negotiation, fallback, combined buttons and sign-extended unused high
bits. Android ARM64 and ARMv7 cores build successfully; device runtime on Android
remains unverified. The separate resampler investigation found no actionable
defect in supported input sizes/rates, so that production code was left unchanged.

A separate 300-frame Tomak runtime probe then confirmed that the device's real
RetroArch enables the bitmask path (`bitmask_enabled=1`); scripted gameplay ran
with all 220,418 audio frames accepted and no ALSA errors or recovery. This is a
short integration check, not an additional performance benchmark. Evidence:
`resume15-pad-runtime-verified.json`.

The production H700 core with the input change is now installed, SHA-256
`69c519d25a9de1d83dbf9b2dce6ffea6db331fecd99e78037bd7d968b2587df6`.
The previous core is retained at `gp32-dev/resume15-installed-core-before.so`;
settings are unchanged. The diagnostic wrappers remain separate developer
artifacts. Deployment evidence: `resume15-core-installed.json`.


## resume16: MMU specialization and exception-return correctness

AArch64 translates memory operations for the MMU enable state captured at
block compilation, removing its per-access control-register load and branch.
CP15 control writes, reset and state loads invalidate the generation; checked
helpers leave the block after a generation change. Memory blocks now use paired
x19/x20 callee saves while preserving the aligned frame and x29/LR chain.
Matched-clock H700 ABBA core throughput improved 2.88% in Her Knights, 3.16% in
Little Wizard and 4.20% in Mill; see [the measured windows and qualifications](GP32_PERFORMANCE_STRATEGY.md).

The new same-VA/different-physical-page regression exposed an existing x86-64
JIT bug: inline memory accesses treated virtual addresses in the RAM/BIOS ranges
as physical even with the MMU enabled. MMU-on byte, halfword, single and block
transfers now use the existing translated helper path. Other native operations
remain enabled. This is a correctness fix and may cost x86 MMU-on performance;
a validated x86 TLB fast path is still future work. The test exercises guest
control writes off/on/off, saved-state restoration in both directions, cold
TLB misses and warm cached blocks.

Separately, ARMv4T exception returns now restore CPSR before aligning the PC.
LDM with S and PC uses the restored SPSR.T state instead of the loaded address's
bit zero. Previously, Thumb SWI/IRQ returns could resume two bytes early or in
ARM state. The fix covers both the interpreter and classified JIT helpers.
The reproducer failed 22 assertions on the original core and now passes for
MOVS PC,LR, SUBS PC,LR,#4 and LDM {...,PC}^ with JIT disabled/enabled.

Final integrated validation: Windows CTest 13/13; native H700 exception return
test and ARM differential test (36,392 events, four expected fallbacks) pass;
Android ARM64/ARMv7 builds pass. One final H700 replay each of the measured Her,
Wizard and Mill windows preserves all seven CPU/video/PCM fields after the
exception fixes. These last replays are exactness checks, not another ABBA
performance claim. Evidence: `resume16-final-exception-h700.json`,
`resume16-final-jit-h700.json`, and `resume16-final-scenes.json` under
`F:/GP32/results/`.

A separate dispatch-counter batching proposal was not retained: no release
speed improvement was demonstrated, and deferring statistics publication could
change observations from public bus callbacks. No production audio policy or
device clock setting was changed in this batch.


### Mill through the actual Spruce/RetroArch path

A diagnostic-only wrapper of the final core replayed the 1,700-frame Mill input
script through Spruce's normal command handoff. The captured final frame shows
the village and Korean resident dialogue at 60.27 OSD fps. This is one endpoint
reading, not a minimum or a full-game claim; it also does not prove that every
headless dialogue tap advanced identically through the frontend.

All 1,249,029 offered stereo frames were accepted by the libretro callback;
zero partial/zero returns and zero pending output after each run were recorded.
ALSA accepted 1,453,824 output frames over 1,893 writes, with zero errors and
zero recovery calls. Core and ALSA totals have different rates and accounting
windows and must not be directly compared. Threaded video reported 1,687 frames
pushed / 14 dropped. Physical speaker quality is still unverified.

The bitmask path and script injection were active on all 1,700 runs. RetroArch
exited normally, MainUI returned, and GP32/shared RetroArch configuration hashes
were unchanged. Diagnostics remain in the developer directory, separate from
the production core. Local records: `resume16-runtime-verified.json`,
`resume16-runtime.log`, `resume16-runtime.png`, `resume16-delivery.json`,
`resume16-alsa.json`, and the external `resume16_audio_probe.c`.

The final production core is installed at `Emu/GP32/gp32emu_libretro.so`,
SHA-256 `759e3d8b690c9e86b50853d5ff33034e9de65695d70d2b8b9f6c6a03588c6cb6`.
The prior core is preserved at `gp32-dev/resume16-installed-core-before.so`;
configuration hashes remain unchanged. Deployment record:
`F:/GP32/results/resume16-core-installed.json`.


## resume17: shifted operands and remaining CPU edge cases

The AArch64 shifted-register optimization and its small matched-clock gains are
recorded in [the performance strategy](GP32_PERFORMANCE_STRATEGY.md). It preserves
per-instruction guest-state publication and all existing slow paths.

Exception entry now honors CP15 c1.V (bit 13): the selected base is zero or
0xffff0000, with the exception offset added. This follows the
[ARM920T TRM DDI 0151C, table 2-10, printed page 2-12](https://documentation-service.arm.com/static/5e8e2a5b88295d1e18d381bb).
The previous core failed both JIT modes of the high-vector SWI oracle by
entering 0x00000008 instead of 0xffff0008. The fixed low/high-vector and
Thumb exception-return cases pass on H700 (`resume17-vector-h700.json`).
Reset still follows the existing explicit reset-vector API; this change does
not claim full MMU fault/abort accuracy or enable a new guest-clock setting.


### ARMv4T instruction-state and privilege rules

Thumb POP PC now retains Thumb state, including even target values, while ARM
LDM/LDR PC retain ARM state and ignore the low two address bits. BX retains its
interworking behavior. This corrects ARMv5-style behavior previously applied
to the ARM920T; the x86 inline LDR path also disagreed with its interpreter.
Both interpreter/classified LDM and x86 native load sites are corrected.
The source rule is [ARM DDI0100I](https://developer.arm.com/documentation/ddi0100/i),
POP A7.1.49 (A7-82/83), LDM A4.1.20 (A4-36/37), LDR A4.1.23 (A4-43/44).
The old LDM tests expecting Thumb entry were corrected to assert word alignment,
ARM state, the loaded registers and the ARM target marker, rather than removing
the assertions. New POP/LDR oracles cover both low-bit patterns and JIT modes.

MSR in User mode now ignores CPSR bits 23:0 while leaving the flag byte writable.
Previously a guest User-mode control write could switch to Supervisor and mask
interrupts. The shared helper fix follows
[DDI0100E MSR, A4-63/A4-65](https://www.cl.cam.ac.uk/teaching/0506/ECADArch/datasheets/arm.pdf).
The external reproducer failed seven assertions before the fix and none after;
the integrated oracle covers immediate/register writes, privileged entry to
User mode and subsequent flag writes. AArch64 only inlines flag-byte writes,
so its control-field operations use the corrected helper as well.

External reproduction records: `resume17-thumb/REPORT.md` and
`resume17-msr/FINDINGS.md` under `F:/GP32/results/`.

### x86-64 MMU-on RAM fast path

The x86 backend now checks the packed TLB entry, full virtual tag, supported
page size, aligned physical base and complete RAM span before inlining MMU-on
loads/stores. A full writable RAM window is required for stores. Cold entries,
BIOS/MMIO, tiny pages, page-crossing block transfers and PC loads retain the
whole-instruction helper. Helpers check PC, status, IRQ/FIQ, generation and
memory-base changes before continuing the trace, and native entry checks its
instruction budget. Win64 shadow space and SysV argument placement are preserved.

A 100-million-guest-cycle MMU RAM loop was compared in eight alternating A/B
pairs against the pre-fast-path x86 emitter. Median throughput was 301.97 vs
889.34 million guest cycles/s (2.95x). Every timing run matched the interpreter's
complete CPU state, RAM and BIOS image. This is a synthetic memory-loop gain,
not an H700 or whole-game speed claim. Artifacts:
`F:/GP32/results/resume17-x64-build/mmu-loop-ab.json` and `x64-emitter.patch`.

Integrated Windows CTest passed twelve tests on its first run. The remaining
ARM JIT case exposed another old ARMv5-style mapped-PC fixture; after correcting
its ARM target and adding state/alignment assertions, the affected test passed.
No production code or assertion was relaxed to make that fixture pass. H700
native differential passes 40,459 events with four expected fallbacks; the
expanded instruction/exception oracle passes too. Android ARM64/ARMv7 builds
pass. Final Windows GP Fight input replay matches the earlier 49ae3d5 endpoint
image byte-for-byte (SHA-256 `2aa4aae04e6461c28ae70a5c1def1fba0966a01cf000519d905287b8f6ba2a3c`).

The final H700 bench also reproduced all seven CPU/video/PCM fields for the
existing Her Knights, Korean Little Wizard and Mill scripts. These were one
exactness replay each, not new A/B performance claims
(`F:/GP32/results/resume17-final-scenes.json`). GP Fight's separately measured
primed ABBA comparison improved median core throughput by 1.86% with exact
state/video/PCM; see [performance strategy](GP32_PERFORMANCE_STRATEGY.md).

### GP Fight frontend and audio delivery

The final integrated core, wrapped only for diagnostic counters/scripted input,
ran the Korean GP Fight match through the normal MainUI -> RetroArch -> MainUI
path for 2400 frames. RetroArch exited successfully after 43 seconds including
startup/shutdown (content runtime 39 seconds); its endpoint overlay reported
60.02 fps. The screenshot shows the classroom scene and Korean HUD names.
The diagnostic overlay was confined to the separate measurement configuration.

All 1,762,032 offered stereo sample frames were accepted, with no partial/zero
returns or pending samples. The ALSA probe recorded zero EPIPE/ESTRPIPE/EAGAIN,
other write errors or recovery calls. This proves delivery in this window, not
physical speaker quality or absence of emulated audio defects. Threaded video
reported 2392 pushed / 9 dropped frames, so this is not a zero-drop claim.
Attack/guard response, later matches and physical input latency remain open.
The normal GP32 launch configuration and user RetroArch configuration hashes
were unchanged. Evidence: `F:/GP32/results/resume17-runtime-verified.json`,
`resume17-runtime.log`, and `resume17-runtime.png`.

The production core (without the diagnostic wrapper) was then atomically
installed at `/mnt/SDCARD/Emu/GP32/gp32emu_libretro.so`, SHA-256
`38726f6f2c9fdbd6e29c7855eb84995fb639198358790aa9cb389cd3c49622f6`.
The previous resume16 core is retained at
`/mnt/SDCARD/gp32-dev/resume17-installed-core-before.so`. MainUI was running,
RetroArch was stopped, and both configuration hashes remained unchanged.
Deployment evidence: `F:/GP32/results/resume17-core-installed.json`.

### Adjacent AArch64 condition flags (resume18)

An unconditional native arithmetic flag producer now marks host NZCV valid for
the immediately following conditional instruction. That one condition can omit
its CPSR load/MSR pair. Guest CPSR remains committed after every instruction;
the fact is invalidated at other producers, memory/helper paths and predicated
joins. No register caching, guest-cycle skipping or cross-block flag state is
introduced. The focused differential adds all fourteen conditions over six
operand pairs and intervening logical, RAM, helper and predicated paths, storing
each decision to RAM so later writes cannot hide a mismatch.

H700 differential passed before and after the optimization: 42,769 events,
four expected fallbacks. Windows ARM JIT regression passed, and Android ARM64
built successfully. Existing compiler warnings remain. Evidence:
`F:/GP32/results/resume18-condition-{before,after}.json`. Qualified H700 ABBA
improves Her Knights combat by 3.42% and the slower Wizard selection window by
3.71%, with identical CPU/video/PCM fields; the latter reaches 60.319 core fps
but still lacks frontend headroom. See [performance strategy](GP32_PERFORMANCE_STRATEGY.md).

The input worker verified the existing same-run GPIO path and retained its
per-run button update: suppressing repeated masks could leak the pad state
restored by a savestate. Its focused input regression passed. The only frontend
change caches the first blank fallback for frontends without frame duplication,
avoiding a repeated 307,200-byte fill when no game is loaded. This does not claim
lower gameplay input latency. The existing libretro audio/video-duplicate test
passed after integration; physical button-to-display latency remains unmeasured.

The audio worker's proposed modulo phase reduction was rejected in parent review:
downsampling can legitimately carry a phase across several tiny input blocks;
reducing modulo the current block invents output samples. The original sources
were restored. The worker's streaming/monolithic ramp check subsequently agreed
across tiny and normal chunks, including 44100 -> 8000/11025 and 11025 -> 44100.
No resampler change or audio speedup is claimed. External evidence is under
`F:/GP32/results/resume18-audio/` (`ramp_law.c` and
`rejected-modulo-phase-owned.patch`); input evidence is under `resume18-input/`.

### Her Knights runtime (resume18)

The CPU candidate ran 2400 scripted combat frames through Spruce's normal
MainUI/RetroArch handoff, exiting successfully after 43 seconds including
startup/shutdown (39 seconds of content runtime). The final capture shows an
attack hitting enemies; the OSD reads 60.64 fps. All 1,763,445 offered stereo
frames were accepted, no partial/zero returns or pending output occurred, and
the ALSA interposer recorded zero write errors or recoveries. Threaded video
reported 2384 pushed / 17 dropped frames, so this is not zero-drop acceptance.

The final OSD also reads 26.14% audio "underrun" and 40.54% saturation. As
explained in [the Her Knights report](HER_KNIGHTS_KOREA_BENCHMARK.md), this is
the frontend FIFO near-empty statistic, not an audible-dropout rate. These
delivery counters and silence counts do not prove physical sound quality.
User configuration hashes were unchanged and MainUI returned. The diagnostic
wrapper predates only the no-game blank-frame cache change; that change was
validated separately by the existing frontend regression and does not affect
this loaded-game path. Evidence: `F:/GP32/results/resume18-runtime-verified.json`,
`resume18-runtime.log`, `resume18-runtime.png`.

The production core including the blank-frame cache was atomically installed
with SHA-256 `3477e6cbe4e59ee273a6eff41471fe23ddb43697d2d344e95409fbc91df26598`.
The prior core remains at `/mnt/SDCARD/gp32-dev/resume18-installed-core-before.so`;
both user configuration hashes were preserved. Android ARM64 and ARMv7 builds
also passed after the frontend integration. Device deployment record:
`F:/GP32/results/resume18-core-installed.json`.


### AArch64 ALU/branch leaf frames (resume20)

Pure native non-register-shift DATA/BRANCH traces now save only x19 in a
16-byte-aligned stack slot. They leave x29/x30 untouched; memory, helper and
other instruction shapes retain the existing full frame. If a supposedly leaf
trace reaches the helper emitter, compilation fails before native publication.
Guest state commits, condition flags and cycle accounting are unchanged.

The native H700 differential passed (42,769 events, four expected fallbacks),
as did arena churn/generation wrap and a direct insufficient-budget native call
that checks no guest state changes. The pre/post-index stack opcodes were
checked against assembler output. Android ARM64 built successfully; this is
build evidence, not Android runtime acceptance. Sol's concrete diff review
found no blocking issue. Evidence:
`F:/GP32/results/resume20-leaf-{jit,recycle}-h700.json`.

A separate audio lifecycle probe found no stale pending PCM across reset,
unload, load or successful state load. Partial batches remain ordered; failed
state load and serialization preserve the pending stream. The existing frontend
audio regression also passed. No audio lifecycle source change was warranted.
This is a scripted frontend-boundary check, not physical sound acceptance.
Evidence: `F:/GP32/results/resume20-audio/evidence.md` and `probe-out.txt`.


The Windows native cache/short-budget regression also passed. Qualified ABBA
measured +2.60% core throughput for Her Knights combat and +0.89% for the
Little Wizard selection window; see the performance strategy for exact inputs,
binaries and clock evidence.

The candidate completed 2400 combat frames through Spruce/RetroArch, exiting
successfully in 43 seconds including startup/shutdown (39 content seconds).
All 1,763,445 offered stereo frames were accepted, with zero partial/zero
returns or queued tails; ALSA reported zero errors/recoveries. Threaded video
reported 2384 pushed / 17 dropped. The final capture shows combat at 60.46 fps.
Its FIFO-near-empty OSD metric reads 35.39%, illustrating that it is not a
physical audible-dropout measurement. Physical sound quality and button-to-screen
latency remain open. Settings hashes were unchanged and MainUI returned.
Evidence: `F:/GP32/results/resume20-runtime-verified.json`,
`resume20-runtime.log`, `resume20-runtime.png`.

The production core was atomically installed with SHA-256
`4aef80826cb348c96231a42e29937bbf555612515f5d064672b3dbda041c6b36`.
The previous core is backed up at
`/mnt/SDCARD/gp32-dev/resume20-installed-core-before.so`.
Installation record: `F:/GP32/results/resume20-core-installed.json`.


### Native dispatcher and I2S source spans (resume21)

Translation and the classified fallback are outlined from native dispatch.
The hot loop also omits two conditions already guaranteed at native publication
and postpones stable-read bookkeeping until portable execution. Sol reviewed
outlining; a separate CommandCode review checked every native-pointer writer,
cache invalidation and immutable bus callback lifetime. H700's outlining
candidate passed the 42,769-event differential, polling and recycle tests.
After the dispatch simplification, Windows arm_jit/arm_poll/recycle and H700
poll/recycle passed. The corrected polling test also passes on both hosts.

The polling regression had a real setup flaw: its callback-absent variant
changed the original bus after CPU creation, although the CPU copies the bus.
It now recreates that CPU before execution and asserts callback presence via
actual query counts. The fastmem stable-poll variant explicitly enables JIT,
ensuring native eligibility cannot silently bypass its stability queries.

The SWE audio candidate proves the complete incrementing/fixed I2S DMA source
span once and reuses its RAM pointer. Partial RAM spans, MMIO and other failed
probes retain per-unit reads and side effects. PCM pairing, FIFO carry, DMA
register updates, sample counts and timing are unchanged. The initially larger
partial-span prototype was discarded; production adds only six lines and
removes two per-unit pointer probes.

The worker initially rejected the small candidate after mixed x86 Clang -O2
results. Parent validation used the actual release optimization level (-O3)
and target hardware: nine alternating timing rounds on H700 measured candidate
runtime ratios 0.7788/0.7664/0.7916 for whole 32/16-bit stream/burst, and
0.8096/0.7846 for single-service 32/16-bit; the untouched byte control was 1.0000.
Thus the measured PCM DMA cases take roughly 19-23% less time. These are isolated
component workloads without a separate clock monitor, not game FPS claims.
Windows Clang -O3 also showed no median regression in those cases, unlike the
worker's -O2 run; short workstation timings are not a global PC speed claim.

The worker's differential harness compared PCM, SoC snapshots and DMA/IIS/IRQ
state across ordinary and boundary cases. Parent's existing s3c2400_timing and
libretro_audio regressions passed on the integrated source. Android ARM64 and
ARMv7 release builds passed. The integrated H700 binary matches all seven
CPU/video/PCM fields in both Wizard selection and Her Knights combat:
`F:/GP32/results/resume21-final-exactness.json`. Final benchmark SHA-256:
`834553a87ff73c75d3cace98520263c704aba8f8c864eb9a29099952d2b3648b`.
Audio evidence: `F:/GP32/results/resume21-audio/time-h700.json`,
`time-pc-o3.json`, and the parent addendum to `evidence.md`.

The independent video-staging prototype remains outside production. Its
real-capture harness crashed before candidate exactness/timing completed;
synthetic baseline-vs-baseline checks cannot validate the candidate. No video
speedup is claimed and libretro.c remains unchanged. The unfinished prototype
and limitation are preserved in `F:/GP32/results/resume21-video/REPORT.md`.

The first RetroArch cold-boot probe remained at the BIOS menu. Its diagnostic
input file used `frame:A`, which `input_script.c` defines as a one-frame tap,
not the intended held-button intervals. That run's silent output is excluded
from gameplay/audio acceptance; it does not indicate a new core regression.
The standalone benchmark uses its own boot pulses and is a separate workload.
Evidence: `F:/GP32/results/resume21-runtime.png` and `resume21-wizard-boot.txt`.
The follow-up runtime uses the existing Korean Wizard combat state and its
explicit `frame:=A` / `frame:=NONE` input script.

The saved-state Wizard run successfully loaded the combat state and completed
2400 frames through Spruce/RetroArch: exit 0, 44 seconds including startup and
shutdown, 39 content seconds. All 1,762,933 offered stereo frames were accepted;
there were no zero/partial returns or queued tails. ALSA reported zero errors
and recoveries. Unlike the excluded BIOS run, most output contains nonzero PCM.
The final screenshot shows the actual match's "You Lost" screen at 56.74 OSD
fps. Video statistics report 2390 pushed / 11 dropped. This is not an all-scenes
60 fps result or proof of physical audio quality. The late zero-PCM interval
and OSD FIFO statistic are retained as observations, not classified as audible
dropouts without a recording. MainUI returned and settings hashes are unchanged.
Evidence: `F:/GP32/results/resume21-combat-runtime-verified.json`,
`resume21-combat-runtime.log`, `resume21-combat-runtime.png`.

The production core was then atomically installed with SHA-256
`d18bd01d9b0b9ebc07dd28a8884687dc32d76b5838e790d0e89a2f7b70253dec`.
The previous core is backed up at
`/mnt/SDCARD/gp32-dev/resume21-installed-core-before.so`.
Settings remained unchanged. Installation record:
`F:/GP32/results/resume21-core-installed.json`.


### AArch64 memory addressing and Android page alignment (resume22)

CommandCode's A64 candidate folds pre-indexed, non-writeback memory addresses
directly into the address register and removes the redundant register copy.
Word-load rotation uses one `UBFIZ` instead of mask-plus-shift; stores no longer
compute an unused rotation count. Writeback order, PC behavior, guest flags,
RAM/TLB guards and helper fallbacks remain unchanged. An ordinary pre-indexed
word load emits two fewer instructions and a word store three fewer; these
counts are not whole-game speed claims.

Parent assembled `ubfiz w11, w0, #3, #2` and verified encoding `0x531d040b`.
The H700 candidate passes the existing 42,769-event JIT differential (4 expected
fallbacks) and native arena churn/generation-wrap regression. The existing
memory tests exercise rotation, byte/halfword extensions, writeback and mapped
RAM/MMIO boundary behavior. No new duplicate test suite was added.
Evidence: `F:/GP32/results/resume22-a64-jit-h700.json`,
`resume22-a64-recycle-h700.json`, `resume22-a64/ubfiz.o`.

The independent portability review identified a build-setting dependency:
Android 64-bit ELF alignment previously relied on the installed NDK default.
The build now explicitly requests 16 KB max/common page alignment for arm64-v8a
and x86_64 Android cores, following the official
[Android page-size guidance](https://developer.android.com/guide/practices/page-sizes).
NDK 28.2 ARM64 and ARMv7 builds pass; every ARM64 PT_LOAD has `Align 0x4000`.
This is a build/ELF check, not Android device-runtime acceptance. No host-page
assumption or publication failure was found in the reviewed A64 JIT path;
manufacturer policy restrictions and actual Android execution remain untested.

SWE's standalone source-audio probe identified a deliberate guest audio stop:
IISCON changes from 0x27 to 0x0e, clearing the start bit after DMA2 drains.
The same standalone replay on H700 produces 407,434 source stereo frames;
its last nonzero frame is 2231 and silence begins at 2232, matching the PC
probe. This explains that replay's source silence but does not establish
physical speaker quality or explain other games' dropouts.

The first libretro source-audio trace initially seemed inconsistent: its last
nonzero frame was 2077, while the endpoint was one guest frame (987,500 cycles)
behind the standalone replay. Frame zero revealed the cause: RetroArch runs a
BIOS frame before loading the auto-state. The diagnostic input sequence had
already begun, so a one-frame input-phase shift changed the fight and its
music-stop time. The wrapper now resets/reloads its input script after a
successful `retro_unserialize`; 2401 total frontend runs allow the initial BIOS
frame plus 2400 replay frames. This is a diagnostic replay correction, not a
production core input-behavior change. The stock libretro source is unchanged.
Evidence: `F:/GP32/results/resume22-audio-h700.json`,
`resume22-final.csv`, `resume22_aligned_probe.c`.

The corrected runtime completed 2401 frontend runs (2400 after auto-state
load). Every replay frame matches the standalone PC probe's guest PC, source
stereo-frame count, nonzero-frame count and IISCON. Its final 5,559,005,000 guest
cycles / PC 0x0c07788c match the H700 benchmark endpoint; total source frames
are 407,434 and the final nonzero source frame is 2231 relative to state load.
This confirms that the earlier source-audio timing difference was diagnostic
input phase, not a demonstrated x64/A64 or audio transport discrepancy.

All 1,763,356 offered frontend stereo frames were accepted, with no partial/
zero returns or pending tails and no ALSA errors/recoveries. The image confirms
actual gameplay with the diagnostic OSD disabled; this time all existing config
keys were replaced, not appended. Average interval over the last 300 frames is
17.17 ms, p99 19.02 ms, so sustained 60 fps everywhere is still unproven.
The two diagnostic changes mean this is not an isolated OSD speed comparison.
Physical sound quality/input latency and Android execution remain open.
MainUI returned and user settings stayed unchanged.
Evidence: `F:/GP32/results/resume22-aligned-summary.json`,
`resume22-aligned-runtime-verified.json`, `resume22-aligned-runtime.png`.

The production core was atomically installed with SHA-256
`8375aea8a795c2025314822ca9f9a4af478496d8ebd5c9677bbdc3baca3a63c9`.
The previous core is backed up at
`/mnt/SDCARD/gp32-dev/resume22-installed-core-before.so`.
Installation record: `F:/GP32/results/resume22-core-installed.json`.


### Unframed leaf calls and redundant LCD clears (resume23)

A diagnostic census of the Korean Little Wizard combat replay (warmup 2100,
measured 300 frames) found 13,959,799 dispatches to the three-instruction abs
leaf at 0x0c077884. The surrounding caller blocks contributed another
21,898,315 dispatches. The instrumented run matches the previous baseline's
seven CPU/video/audio exactness fields; its slower timing is not a benchmark.
Evidence: `F:/GP32/results/resume23-hot/summary.json` and `wizard.json`.

The translator now stitches small register-only BL leaves ending in an
unconditional MOV pc,lr or BX lr. It retains every guest instruction and
original PC, the real LR write, cycle budgets and code-fetch revalidation.
Speculative new leaf reads use existing direct mappings without MMIO callbacks
or page-table walks. SP/LR writes and conditional returns reject inlining.
The native MOV return guards the actual aligned target before continuing;
BX retains the checked helper and interworking. This is a generic ARM path,
not a patch to a game's address or clock. Both x64 and A64 emitters support it.

LCD scanout no longer clears 307,200 bytes before a confirmed full 240x320
rewrite. Partial, unsupported and fallback scanouts retain the black clear.
The decision uses the accepted contiguous span in the renderer itself, without
duplicating its range/stride checks. The existing LCD differential harness
passes 868 cases with identical framebuffer and DMA/frame state.

H700 passes the complete existing JIT differential with the new leaf cases
(110,818 JIT events), native arena churn/generation wrap, and stable-poll
regressions. Sol's focused x64 leaf differential and exception/cache checks
also pass. Windows and Android ARM64/ARMv7 cores build; Android runtime remains
unverified. Evidence: `F:/GP32/results/resume23-jit-h700.json`,
`resume23-recycle-h700.json`, `resume23-poll-h700.json`, `resume23-leaf/`,
and `resume23-lcd/`.

The clock-qualified Wizard ABBA comparison improves median unrestricted core
throughput from 70.6235 to 91.89 fps (+30.11%). All seven CPU/video/audio fields
match, with 14 measured clock samples at 1512 MHz. This combines the leaf and
LCD changes; it is not a per-change attribution or an all-games/display-fps
claim. Evidence: `F:/GP32/results/resume23-wizard-late-abba.json`.

CommandCode's separate libretro audio review found no demonstrated defect to
change. Its existing focused audio test passed; a scratch carried-phase
resampler capacity probe found no writes beyond the declared output bound.
The production libretro source, latency settings and stall policy are unchanged.

Her Knights Korea combat (1200 warmup / 1200 measured) also preserves every
CPU/video/audio field across ABBA. Candidate runs measured 122.320/122.495 fps,
but baseline clock ramping introduced 1320/1416/1512 MHz samples, so this
sequence is explicitly disqualified as a speedup claim. It remains exactness
evidence. No governor setting was changed and no repeat was needed to establish
that result. Evidence: `F:/GP32/results/resume23-her-abba.json`.

The aligned real RetroArch replay completed 2401 frontend runs (2400 after
state load), with every replay frame's guest PC, source-audio count/nonzero
count and IISCON matching the standalone reference. All 1,763,356 offered
stereo frames were accepted; zero/partial returns, queued tails, ALSA errors
and recoveries were all zero. The final gameplay image was visually checked.
Settings hashes are unchanged and MainUI returned normally.

In that run the late-300 average core time falls from the previous run's
13.014 ms to 9.879 ms; the candidate maximum core time is 12.930 ms.
Frontend intervals average 16.488 ms but p99 is 28.125 ms (previous run:
17.166/19.021 ms). The longest calls spend roughly 11-18 ms in the frontend
audio callback after the guest disables IIS; threaded video reports 2383
pushed / 19 dropped frames. These single runtime captures are not a qualified
pacing A/B. They do establish that residual stalls are outside guest CPU work
in the observed calls; physical sound quality, end-to-end input latency and
steady 60 fps across all scenes remain open. No latency/governor/volume setting
was altered. Evidence: `F:/GP32/results/resume23-aligned-summary.json`,
`resume23-aligned.csv`, `resume23-aligned-runtime-verified.json`, and
`resume23-aligned-runtime.png`.

The production core was atomically installed with SHA-256
`f511b8c5719be6519255e1d9585bf105b5030f73aa0719da84cf69071981431f`.
The previous core is backed up at
`/mnt/SDCARD/gp32-dev/resume23-installed-core-before.so`.
Installation record: `F:/GP32/results/resume23-core-installed.json`.


### Callback pacing experiment and proven leaf returns (resume24)

An external diagnostic core moved audio submission/flush before the video
callback while freezing all guest core code at 7f99e06. The same 2400-frame
Wizard replay still matched guest/source-audio state on every frame and all
1,763,356 offered frames were accepted, with no ALSA errors or recoveries.
However, late-300 intervals above 20 ms increased from 5 to 10, p99 rose from
28.125 to 31.982 ms, and threaded-video drops rose from 19 to 28. This does not
establish a general order-dependent regression, but provides no reason to
adopt the change. The experiment was rejected; production callback order and
all user settings remain unchanged. Evidence:
`F:/GP32/results/resume24-pacing/comparison.json`,
`resume24-aligned-summary.json`, `resume24-aligned-runtime-verified.json`.

The installed frontend identifies itself as RetroArch 1.22.2 / 69a4f0e.
That exact upstream [alsathread source](https://github.com/libretro/RetroArch/blob/69a4f0e/audio/drivers/alsathread.c)
waits for FIFO space in its blocking write path and drains one ALSA period on
a worker. This is consistent with the observed callback waits, not proof of
a core defect or permission to replace the frontend/change its buffering.

A separate checked-helper census found zero ordinary or inlined BX calls in
the measured Wizard selection and Her Knights combat windows. Together with
the previously rejected general BX candidate, this rules out prioritizing
that path for these workloads. No BX-only production patch was added.
Evidence: `F:/GP32/results/resume24-bx-census/{wizard,her}.json`.

The retained return optimization keeps PC materialization and only removes
the redundant LR load/alignment/target guard when a decoded caller successor
follows a proven register-only leaf. The guest return op and cycle count stay
in the trace; returns at a trace boundary retain their guarded path. Review
rejected an earlier PC-store-elision prototype because x64 MMU-off MMIO
callbacks could observe a stale PC. That prototype was never installed.

The final H700 candidate improves qualified Wizard late-window ABBA median
throughput from 91.3025 to 92.999 fps (+1.86%), with all seven CPU/video/audio
fields matching and twelve measured clock samples at 1512 MHz. The earlier
2.58% prototype result is superseded and is not a shipped speedup. The full
A64 differential passed before the PC-store correction; the affected leaf
subset was rerun after it and passed (68,050 events). Evidence:
`F:/GP32/results/resume24-return-final-abba.json`,
`resume24-return-jit-h700.json`, `resume24-return-final-leaf-h700.json`.

SWE found an independent IIS scheduling inconsistency in byte-sized DMA2:
each byte transfer already pushes a FIFO halfword, but the scheduler requested
four transfers per stereo-frame tick. It now requests two, as in halfword DMA.
This prevents two output frames per declared sample period and an early
terminal-count IRQ. A focused regression demonstrates the old 8 frames/2 IRQs
versus the expected 4 frames/1 IRQ after four ticks. H700 passes the corrected
case and the prior LCD/PLL trace remains 47779e5037cd7b27. No state layout or
16/32-bit DMA pacing changed; this is not a claim that the measured games'
audible issues were caused by byte DMA. Evidence:
`F:/GP32/results/resume24-iis-h700.json`, `resume24-iis/`.

The return review also exposed a pre-existing x64 MMU-off callback-PC bug.
Native word/byte/halfword load/store helpers could call MMIO with the PC of
an earlier instruction. They now materialize the executing op's PC+4 only on
the helper branches, leaving direct RAM/BIOS hits unchanged. A callback that
returns/records the CPU PC reproduced 26 baseline mismatches; the fixed
sequence matches exec_arm_at for all eight load/store variants after both
MOV and BX leaf returns. The full x64 differential passes 110,874 events;
H700's affected callback subset also passes. Evidence:
`F:/GP32/results/resume24-x64/{callback-before,callback-after,differential-after}.log`,
`resume24-callback-pc-h700.json`.

Final real RetroArch replay again matches every guest/source-audio frame,
accepts all 1,763,356 stereo frames without pending tails, and reports no ALSA
errors/recoveries. Late core time averages 9.687 ms; frontend interval p99
remains 31.880 ms, so the residual pacing issue is not declared fixed.
MainUI returns normally and settings hashes are unchanged. Windows and Android
ARM64/ARMv7 cores build successfully. The later x64-only source changes alter
A64 debug metadata; the measured and final relinked probes are byte-identical
after removing debug sections (SHA-256 45f6a97084e4bb94bc167a53962233fb45116ee47c0e00425ab6e582fd89d772),
so the completed runtime evidence is reused. Evidence:
`F:/GP32/results/resume24-final-aligned-runtime-verified.json`,
`resume24-final-aligned-summary.json`, `resume24-return/relink-parity.json`.

Installed production SHA-256:
`46fdcec8668efcb497665e7ef702f44e2a682bda4bf67eaeac30ffaace8e935f`.
Backup: `/mnt/SDCARD/gp32-dev/resume24-final-installed-core-before.so`.
Record: `F:/GP32/results/resume24-final-core-installed.json`.


### Native branch boundary correctness (resume25)

A long stitched branch chain exposed an x64 epilogue bug: when an unconditional
branch occupies the final trace slot, the emitter previously committed the
instruction's fallthrough rather than its branch target. A 128-branch chain
that skips ADD traps reproduces r1=1 in native execution versus r1=0 in the
interpreter. The epilogue now commits the unconditional branch target, matching
the existing A64 behavior. The full x64 differential including this regression
passes (110,940 events), and the new chain case passes on the current H700
baseline. This fixes control flow; it is not a game-level speedup claim.
Evidence: `F:/GP32/results/resume25-native/NOTES.md`, `chain-before.exe`,
`chain-after.exe`, and `resume25-chain-h700.json`.

Separately, removing an intermediate A64 PC store for stitched B/inlined BL
passed native differential and all seven game exactness fields. A qualified
1512 MHz ABBA measured 92.919 -> 93.456 median fps (+0.58%), but candidate runs
varied 92.763/94.149 fps. This does not establish a useful improvement; that
candidate remains outside production. Evidence:
`F:/GP32/results/resume25-native-abba.json`, `resume25-native-jit-h700.json`.

### Failed state loads preserve the live machine (resume25)

The libretro lifecycle probe reproduced a previously documented limitation:
loading a truncated state returned false but had already restored CPU state.
The SoC also committed SmartMedia before reading the final queued audio, so a
truncation there could change the card even when RAM/audio remained untouched.
The loader now keeps the fixed CPU image pending until the SoC succeeds, and
stages a new SmartMedia device until every RAM/card/audio read succeeds.
The v0002 byte layout is unchanged. This requires no extra whole-machine
snapshot or RAM/card copy on successful loads. A temporary 53 KiB CPU wire
image and a small card object are added to the allocations the loader already
performed. CPU staging uses the heap so it does not overlap the already large
SoC image on the default Windows thread stack (identified in SWE review). The read-only
file loader does not report a completed state as rejected merely because
closing the already-consumed input stream fails.

The existing H700 Korean Little Wizard combat state loads and replays with all
seven CPU/video/audio exactness fields unchanged. Android ARM64 and ARMv7 core
builds pass. Evidence: `F:/GP32/results/resume25-state/summary.json` (pre-fix
reproduction), `resume25-state-wizard-h700.json` (valid existing state replay).

The existing frontend probe now reports `mutated_advanced_machine=0` after a
rejected truncated load (previously 1). Valid unserialize/reset still invalidate
video, serialization capacity remains stable, and unload/reload works. Its
`partial_sections_applied` label only compares with the older saved snapshot;
it is not a mutation check and remains 1 because the live machine is preserved.
Evidence: `F:/GP32/results/resume25-state/probe-after.log`.
Before moving the CPU staging buffer from stack to heap, a qualified 1512 MHz
ABBA preserves all seven exactness fields and measures
93.023 baseline vs 93.262 candidate fps; this is performance preservation, not a
claimed optimization. The earlier cold-run 67.592 fps was not clock-qualified;
the primed baseline likewise started at 66.698 fps. Evidence:
`F:/GP32/results/resume25-state-abba.json`.

The synthetic whole-machine regression uses different CPU progress, RAM,
SmartMedia data/device state, and nonempty queued PCM. The frozen baseline
fails state preservation for a RAM truncation and the post-card audio gap;
the final loader passes all component-boundary/mid-audio truncations and both
memory/file valid-load round trips on H700. Evidence:
`F:/GP32/results/resume25-state-regression-before-h700.json`,
`resume25-state-regression-h700.json`.

The final fixed-snapshot regression passes on both Windows and H700; Windows
libretro audio and persistence tests also pass. Windows/Android ARM64/ARMv7
cores rebuild after the staging-buffer allocation change. Evidence:
`F:/GP32/results/resume25-state-regression-final-h700.json`,
`F:/GP32/results/resume13-input-build/Testing/Temporary/LastTest.log`.

Installed production core SHA-256:
`340189dde99d73413fc7ebf9ed01fffd63eee356204cd482706512ece726cc42`.
The prior core is preserved at
`/mnt/SDCARD/gp32-dev/resume25-installed-core-before.so`. GP32 launcher and
RetroArch settings hashes are unchanged; MainUI was running and RetroArch
was stopped during the atomic core replacement. This round did not repeat
physical audio/input-latency acceptance or claim all games are validated.
Record: `F:/GP32/results/resume25-core-installed.json`.
