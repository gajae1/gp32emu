# Development return point

Updated 2026-10-05. Read this before repeating investigations; verify Git and
device state before using snapshots. Goal remains full low-end compatibility,
performance, audio and input correctness across the library, not boot-only tests.

## Current accepted work

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
below record the earlier prototypes and remain for history. Open limits: SDK
task switches inside callbacks fault, HLE mixing pauses during callbacks, and
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
