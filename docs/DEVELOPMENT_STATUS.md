# Development return point

Updated 2026-10-05. Read this before repeating investigations; verify Git and
device state before using snapshots. Goal remains full low-end compatibility,
performance, audio and input correctness across the library, not boot-only tests.

## Current accepted work

- Round 152-153 (`783a6a2`, `53d7619`, installed on H700, core sha256
  `437fc417…`): slimmer GPIO/identity IO writes (H700 ABBA 1.512 GHz, identical
  output: Blue 75.47 -> 75.94, LGM 60.12 -> 60.94, Princess 109.56 -> 110.54 fps)
  and SmartMedia savestate deltas against the loaded .smc (state v0014, about
  10.4 MB instead of 26.6-43.9 MB, save+load 1.7 ms instead of 6-12 ms; v0002+
  still load). Non-profile H700 frame times at `53d7619` (`--frame-times`):
  LGM 60.7 fps, p50 24.3 / p99 25.6 / max 51.1 ms, 715/1200 frames over
  16.67 ms; the mean (16.5 ms) implies a bimodal split of heavy ~24 ms guest
  logic frames and light ~5 ms wait frames. Pairs fit 33.3 ms, but each heavy
  frame must get about 1.46x faster before every host frame meets 16.67 ms
  and video pacing stops depending on the audio buffer. Princess 111.9 fps p99 9.4; ASR 198.6
  p99 14.4 (3 over); Her 172.5 p99 7.2; Blue 75.0 p99 21.9 (24 over, max
  30.6). Evidence `F:/GP32/results/round153/h700-frametimes.json`.
- Rejected (round 153): LCDCON5 direct case in `s3c2400_read32_io` plus
  compare-instead-of-modulo LCD phase. LGM reads LCDCON5 ~15k times/frame
  and writes GPIO ~27k times/frame (27-game MMIO audit,
  `F:/GP32/results/round153/w-io-audit/`), but H700 ABBA was within noise
  (LGM 61.09 -> 61.17, Princess 110.71 -> 110.83, Blue +0.5%) with identical
  output. The remaining IO cost is the checked-helper exit and dispatcher
  re-entry, not register decode.
- Rejected (round 154): deferring BL superblock continuation until the head
  block is entered 64 times (dispatch fast-path hot counter plus one
  retranslation). It cut Blue's extended code 3.13 MiB -> 0.36 MiB on PC with
  identical output, but H700 ABBA lost Blue 75.07 -> 73.91, LGM 60.75 ->
  57.81, Princess 110.49 -> 105.80 fps: the counter on the hottest dispatch
  path costs more on the A53 than the larger code footprint. Blue's -1.1% from
  `1962e10` stays. Evidence `F:/GP32/results/round153/w-blue/`,
  `F:/GP32/results/round154/abba-hot/`.
- `2f494c1`: direct-HLE SWI 4 and exception vectors park instead of running
  into data (vba32 exit, gpmadmp3, fgen32, gpfrodo, race_ngp, handyport2 stop
  executing unmapped memory; gplynx now updates its screen). Commercial scenes
  identical; native H700 jit/exception/callback/wait/state tests pass.
- `4c8c3ee`: direct mode carries the retail IRQ dispatcher/FIQ clear, SWI
  9/0x0a install/remove ISR-table handlers, SWI 4 and reset/fault vectors
  restart the loaded image at a frame boundary. 425-image homebrew sweep
  (FXE 384, FPK 12, GXB 29; 900 frames, JIT and interpreter): OK 329 -> 411,
  parked 72 -> 0, runaway 21 -> 11, load failures 3, JIT mismatches 0, no
  regressions. Remaining: acidwarp/gpcine2p static screens, 11 runaways,
  gpcheat.fxe/ubook.fpk/slubFW.fxe load failures. Table:
  `F:/GP32/results/round154/w-hb2/result.md`.
- LGM workload anatomy (round 153, `F:/GP32/results/round153/w-smc/`):
  every frame uses its whole guest budget; 23.8% of guest instructions are the
  game's SmartMedia GPIO bit-bang driver (one NAND byte per command, re-reading
  a few pages: page 16512 101k times), 4.6% the BIOS LCD sync-edge poll at
  0x2740. ~46k SoC word accesses per frame, ~53 per NAND byte. Caching card
  flags/readback words in the SoC kept output identical but was neutral on
  H700 (LGM 60.95 -> 60.79, Blue 75.42 -> 75.96, Princess 110.63 -> 110.68);
  not merged. The guest's own instruction stream dominates; remaining lever is
  dispatch/helper overhead per access.
- Audio pops (round 150): the BIOS boot pop, the Dooly Soccer start pop and
  the BIOS sound-menu tick are guest PCM. The menu tick is three 335-sample
  plateaus of exactly 0x8000 that the Europe v1.6.6 BIOS reads from its own
  ROM asset at 0x4bf04; the core inserts no step, fade, silence mismatch or
  stale FIFO data. No emulator change; evidence `F:/GP32/results/round150/dooly-pop/`.
- Savestates (round 150): serialize size is constant per game, rejected
  loads never mutate live state, JIT restore is deterministic, per-frame
  serialize has no side effects. States are 26.6-43.9 MB because the whole
  SmartMedia image is stored; a delta format is being redesigned so states
  from earlier sessions stay loadable after the card is rewritten.
- Measurement note (round 150): `build-h700-resume2-profile` sets
  `GP32EMU_CPU_PROFILE=ON`; its counters cost real time in IO-heavy scenes.
  Little Girl Mill gameplay (cold BIOS boot, warmup 2400, 1200 frames, H700
  1.512 GHz, `9934560`) runs 30.98 fps in that build but **45.6 fps** in a
  plain release bench (`GP32EMU_CPU_PROFILE=OFF`, what the installed core
  uses). Profile-build ABBA still ranks candidates; absolute device fps must
  come from a non-profile build. Bisect 69fe388/e728b9b/9934560 showed no
  regression (30.2/30.1/31.0 profile-build fps, identical output).
- `9934560`: direct dispatch fast path for plain native blocks (A64 hot path
  80 -> 64 instructions); H700 ABBA Princess +4.5%, Blue +3.0%, LGM +2.9%.
- Round 140-149 commits: libretro-only ELF exports; SDK task switch inside
  direct-HLE callbacks (state v12); x64 JIT natives for leaf frames, logic and
  carry arithmetic with flags, LDR pc, MSR/MRS, PC-writing data ops; predicate
  -failed terminal ops commit PC (homebrew JIT divergence); non-cacheable PCs
  interpreted; big.LITTLE-safe A64 cache maintenance; AArch64 8bpp scanout STP
  (H700 ASR +2.6%, Her +2.3%); SWI 5 image placement and SWI 0x1FF in
  direct-HLE; parked SDK scan prefilter. Library sweep: 26 commercial SMC run
  3600 frames with byte-identical JIT/interpreter state at frame 1200.
- AArch64 address materialization uses shifted ADD immediates for CPU fields,
  reducing generated instructions while native H700 differential/recycle
  checks pass. See `A64_ADDRESS_MATERIALIZATION.md`; no FPS gain is claimed.
- Fixed-capacity two-way JIT cache: fewer retranslations without enlarging the
  block table. The initial two-pass lookup regressed H700 ASR and was replaced
  with a first-way fast path. Final bounded Blue/ASR guest outputs match;
  Blue translations fall 10,463 to 3,983 and arena recycles 1 to 0. Throughput
  remains roughly unchanged; see `JIT_CACHE_ASSOCIATIVITY.md` for limits and
  rejected measurements. Four C23 release architectures build; not installed.
- `c7612ec`: state restoration rejects invalid IIS FIFO/negative IIC indices before
  mutating the live SoC, and normalizes old IIC data-phase counts without
  changing transaction behavior. The defect previously allowed later register
  writes to escape their arrays. A real-subsystem regression failed before
  the fix; five focused state, EEPROM, IIS and persistence checks pass.
- `4040413`: INTMOD routes peripheral sources to FIQ; IRQ arbitration excludes
  them; CPU line state is reconciled on reset/attach. PC focused checks and
  H700 routing/exception checks pass. Four release architectures build.
- `fa339bf`: CPU runs stop at PWM expiry; timer writes settle elapsed time
  before applying. Fine/coarse ARM handler results agree.
- `a61da73`: IIS DMA completion deadlines avoid delaying guest refill IRQs.
  Bounded ASR cold-start missing PCM frames decreased from eight to zero.
- No claim of full gameplay, original-hardware sound, latency or Android
  runtime acceptance follows from these checks.

## Direct-HLE display wait and resumable callbacks (integrated)

Resumable callbacks and the guest-loop display wait are now in production;
see the integrated section at the end of `HLE_WAIT_INTERRUPTS.md`. The notes
below record the earlier prototypes and remain for history. SDK task switches
can now suspend and resume a callback through its guest task frame; state v12
records that ownership while v11 and older states still load. Open limits:
HLE mixing pauses during callbacks (including task suspension), and
WinterSports Eins alpha still leaves RAM.

Private candidate/evidence on the development machine:
`F:/GP32/results/resume131-wait/`:

- `gp32-candidate.c`, `wait-probe.c`, `wait-after-fiq.log`: guest ARM wait loop;
  16 short IRQ/FIQ/interpreter/JIT/ARM/Thumb/legacy-entry combinations pass
  after the accepted FIQ fix. Candidate is **not** `src/gp32.c`.
- `design.md`: Sol design review. Reject low32 cycle comparison: a task
  suspended beyond half-range can wait again after its deadline has expired.
- `fixtures.md`: two existing direct-FPK candidates and previous commands;
  display-trampoline hits are unverified, OMG is not locally located.

`F:/GP32/results/resume132-wait/` contains a wide-nanosecond private candidate
and a callback continuation design. Sixteen short wait cases pass, but wide
state serialization, long task suspension and resumable callbacks still need
implementation/validation. It has not been installed on the device.

An independent callback-return identity fix now requires an active callback
and its actual private return-stub PC. A real ARM exception fixture failed
before the guard in interpreter/JIT and passes afterward; the four focused
PC timer/PCM/file/state checks pass. The H700 timer check also passed from RAM
with eight protected hashes unchanged. See `HLE_WAIT_INTERRUPTS.md`.

Next steps: use a per-call wide elapsed-time deadline and coherent time reads;
settle the display instruction before assigning its deadline; keep normal
guest exception/stack semantics. Resolve synchronous HLE callback exhaustion
without discarding a suspended callback, and retain explicit old-state wait
compatibility. Then run focused correctness checks and two bounded direct-mode
scenes. Do not promote the existing low32 prototype or claim callback support.

## Device and execution constraints

H700 device last checked at `192.168.0.204`, SSH user `spruce`. The user-requested
SD repair is complete: `/dev/mmcblk0p7` was normally unmounted, its inconsistent
FAT and three broken chains repaired, affected files restored from PC backup,
and an offline read-only check exited zero. All 555 backed-up regular files and
eight protected hashes matched. Write/read/delete passed; a clean reboot
restored stock MainUI/SSH, with rw mounts and no new FAT/I/O errors. Original
failure cause/card physical health are not established. Local evidence and
backups: `F:/GP32/results/device-sd-repair-20261005/`. Installed core remains the
earlier `b4a544e` build. New tests use RAM `/tmp` only, guarded against running RetroArch and
`/tmp/cmd_to_run.sh`, with protected hashes from
`F:/GP32/results/resume114-live-mmio/installed.json` checked before/after.

Keep physical volume, mixer, governor, stock launcher and RetroArch settings
unchanged. Run Zig builds sequentially. PC profile build:
`F:/GP32/results/resume13-input-build`; H700 tests: `build-h700-resume2-profile`;
release builds: `build-win64-c23-resume7`, `build-h700-resume2`, and
`build-android/{arm64-v8a,armeabi-v7a}`.

## Parallel development round

Twenty bounded workers were dispatched at max reasoning; sixteen stopped with
provider 429 errors. An alternative-provider A64 worker also hit 429. State,
loader and ZIP workers completed; their fixes are integrated. Long raw-GXB
labels now use bounded title copying, and dotted ZIP directory names are no
longer falsely rejected. Both failed focused regressions before their fixes;
the combined file check now passes. The resampler worker found no actionable
defect or proven beneficial change; its extensive private checks are worker
evidence, not blanket game/audio acceptance. A requested scratch-log deletion
was blocked by automatic review and was not retried.

The original JIT cache worker completed a fixed-capacity two-way cache candidate.
Parent integration corrected its native-block assertions and removed its H700
dispatch regression. Bounded Blue Angelo/ASR comparisons are complete under
`F:/GP32/results/round135-cache/`; this is not installed. Validation scope is in
`JIT_CACHE_ASSOCIATIVITY.md`.
Private patches/reports belong
under `F:/GP32/results/round133-parallel/`; they must not edit tracked source,
run shared builds, use SSH or delegate. A subsequent Sol max worker owns only
`F:/GP32/results/round134-callback/` for the resumable callback candidate.
Eight further max-effort performance workers were dispatched under
`F:/GP32/results/round134-performance/`; four ended with provider 429, while
A64 codegen completed and its CPU-field address change passed native H700
checks. LCD's unchanged-row candidate was rejected and removed after actual
H700 16-bit scanout regressed about 2.67x despite PC improvements. A NEON
equality refinement repaired the isolated scanout cost, but Princess full-core
throughput still regressed at unchanged frequency; moving the cache to the
SoC tail did not fix that. Both refinements were removed and this worker is
closed. Evidence: `round136-lcd`, `round137-lcd-neon` and
`A64_ADDRESS_MATERIALIZATION.md`. IIS completed with no justified change:
its measured PC cost was below 0.17% in four scenes. The memory-bus candidate
was audited, then rejected after a 2.6% H700 Princess regression (see
A64_ADDRESS_MATERIALIZATION.md). Round 138 workers (Blue load, audio pops,
BIOS stalls, ASR opening, Princess profile) report under results/round138/. Sol completed a
resumable callback candidate, but it faults on callback display waits and must
not ship alone; that worker is now integrating the wide guest wait and state
compatibility. Their IDs are in the private dispatch JSON files. Record final
outcomes before a handoff.

## Broader remaining evidence

See `PC_LIBRARY_COVERAGE.md` for bounded scenes, not blanket compatibility.
GP Fight's gray capture was followed to GAME OVER/title on the prior probe
binary; it was not a proven hang. Default-BIOS GlooP/Pinball/Tears loading,
unattributed controls, long gameplay, physical audio/input latency and Android
runtime remain open. FIFO-empty electrical output is not settled by manuals;
do not invent replacement samples as a fidelity fix.

- Optional guest CPU speed (`CPU_SPEED_OPTION.md`): instruction clock only,
  peripheral time preserved, default 100%. Blue Angelo's CPU-bound NPC load
  shortens at 150-300%; timer-paced titles are unchanged. Round 138 found the
  Blue pause is guest SmartMedia/ECC work (no emulator wait) and the GlooP/
  Pinball/Tears stall is a defect of BIOS image ecf63b73bbd1 with cards lacking
  GAME\\; dated v1.6.6 images boot them (`results/round138/`).
