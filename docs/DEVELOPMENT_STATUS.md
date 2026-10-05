# Development return point

Updated 2026-10-05. Read this before repeating investigations; verify Git and
device state before using snapshots. Goal remains full low-end compatibility,
performance, audio and input correctness across the library, not boot-only tests.

## Current accepted work

- Rejected (round 163): splitting the JIT dispatcher into a compact
  `arm_jit_run` steady state plus ARM_NOINLINE `arm_jit_run_slow` (w-runloop).
  The resident A64 dispatch loop shrank 308 -> 196 bytes and PC improved
  ~3%, but H700 ABBA against the accepted page-crossing build was mixed:
  LGM 79.29 -> 79.64, Princess 119.82 -> 119.90, Blue 85.60 -> 85.14 and the
  ASR opening 95.58 -> 94.42. Outputs identical; the out-of-line slow-path
  calls cost more on A53 than the saved loop lines in the return-heavy scenes.
  Evidence `F:/GP32/results/round163/w-runloop/`, `abba-runloop/`.
- Accepted (round 163): straight-line JIT traces may cross the 1 KiB collector
  page boundary when the next page is already mapped. The side-effect-free
  peek proves the fetch without a table walk, TLB refill, fault or MMIO read;
  revalidation still re-proves every recorded fetch. This keeps hot
  page-straddling loops as one block with one in-place backedge instead of two
  dispatcher entries per pass (ASR's audio/crypto loops at `0x0c064708`,
  `0x0c064734`, ...). PC ctest/pc-parity/parity5/parity-boot identical, H700
  native tests pass. H700 ABBA: ASR opening 93.76 -> 96.02 fps (+2.4%), LGM
  78.76 -> 79.27 (+0.6%), Blue 85.31 -> 85.48; Princess 121.91 -> 119.68
  (-1.8%, still about twice the 60 fps target). Evidence
  `F:/GP32/results/round163/abba-xpage/`.
- Frame-time anatomy (round 163, `gp32_bench --frame-times-raw` prints every
  frame's host time and, with `--cpu-profile`, per-frame counters). H700
  1.512 GHz: the LGM bench scene (cold boot, warmup 2400) is a card-loading
  phase for its first 712 frames (~105k native blocks per frame of SmartMedia
  GPIO streaming, 19.2-20.4 ms each) followed by gameplay at 2-5 ms; the
  "712/1200 over 16.67 ms" figure is that loading run, not gameplay. The ASR
  opening movie is the sustained overload: from
  `F:/GP32/results/round163/asr-opening.state` (headless, 1505 frames from
  cold boot) frames 8-90+ run 77k-128k blocks each and take 17-29 ms on H700
  (2-7 ms on PC), which matches the reported opening crackle. H700 cycle
  sampling of that window: JIT code ~38%, `arm920t_run` 18.8% (loop head,
  lines 6-12), IO word helper 6.4%, `io_write32` 4.2%, LCD render 2.2%;
  the bench's own byte-wise FNV frame hash is ~15% there (absent in the
  core). `F:/GP32/results/round163/device-abba.py` adds this scene to ABBA.
- Rejected (round 161): a fast-transition chaining loop inside
  `arm_jit_run` (enter the next cached block without returning to the outer
  dispatcher). Identical output, but H700 ABBA lost Princess 121.8 -> ~120.0
  (-1.5%) while LGM gained only +0.3%. Evidence `round161/abba-chain/`.
- Rejected (round 161): A64 flag commit by byte store `strb [cpsr+3]`
  (w-flags P1). Identical output, but Princess 122 -> 117.5 when all MSR-f
  went through the helper and 122 -> 119 with a runtime nibble guard; LGM
  79.1 -> 76.7. Likely an A53 store-to-load forwarding stall when the word
  load of cpsr follows the byte store. Evidence `round161/abba-flags/`,
  `abba-flags2/`.
- Rejected (round 161): shared A64 memory-access thunks (w-memthunk): hot
  memory-site bytes -35%, emitted hot footprint -15..-22%, but +2.07
  executed words per access. Identical output; H700 ABBA LGM 78.94 -> 76.66,
  Princess 122.40 -> 118.47, Blue 85.47 -> 85.17. On the A53 the extra
  instructions cost more than the saved I-cache lines. Evidence
  `round161/w-memthunk/`, `abba-memthunk/`.
- Rejected (round 161): the remaining w-flags parts without the byte store
  (logical flag commit through cmp/cmn + bfxil/bfi with carry kept at bit 0,
  and `msr nzcv` published before a following conditional test). Identical
  output; H700 ABBA Blue 85.18 -> 86.27 but LGM 78.75 -> 77.67 and Princess
  122.06 -> 120.66. Evidence `round161/abba-flags3/`, patch
  `round161/w-flags/`.
- Accepted (round 161): x64 dense emission (w-x64dense, `src/arm920t.c`
  only): emitted x64 code -4.7..-6.7%, PC LGM +3.0%, Blue +1.1%, Princess
  neutral; PC ctest, pc-parity, parity5 and parity-boot identical. H700
  unaffected.
- Rejected (round 161): splitting io_write32 into a short GPIO body plus
  out-of-line GPEDAT and IRQ/DMA/LCD halves (aarch64 1508 -> 436 bytes, 17 ->
  7 GPIO lines). Identical output, but H700 ABBA lost LGM 78.95 -> 78.52 and
  Princess 121.97 -> 121.43: LGM's SmartMedia streaming writes GPEDAT
  (command/address latches) constantly, so the extra call costs more than
  the saved lines. Evidence `F:/GP32/results/round161/w-chelp/`, `abba-iow/`.
- Input latency audit (round 161, `F:/GP32/results/round161/w-input/`):
  measured button-edge-to-changed-frame on Blue, Princess, ASR, Her and LGM;
  every scene sits exactly at its guest floor (first GPIO read that sees the
  edge, then the game's own draw and SADDR flip). No late poll, no extra
  presented-frame delay, no double buffering. The proposed mid-run input
  re-poll was not taken: Spruce's RetroArch uses the default Late poll type,
  where the core's input_poll callback is a no-op and input is snapshotted
  once per frame, so it would add callback cost for no gain.
- Round 161 dispatcher hit path (`arm_jit_run`, all backends): the block's
  {generation, epoch} tag is compared with the CPU's adjacent pair in one
  64-bit load, the fast-path metadata (native_ok, counted poll, deferred
  inline) is one 64-bit load and mask, and block hits count in a register
  published once per run. Output identical (PC pc-parity/parity5/boot, H700
  ABBA). H700 1.512 GHz: tag compare Princess 116.50 -> 120.82 (+3.7%), LGM
  76.50 -> 77.82 (+1.7%), Blue 84.33 -> 84.50; then metadata + hits on top
  Princess 120.93 -> 122.26, LGM 77.69 -> 78.82, Blue 84.81 -> 85.33.
- Round 161 GPMM-layout freeware cards: the retail BIOS launcher only boots a
  card's top-level GAME\ executable, so cards whose executable sits elsewhere
  (Dynamate, GlooP Deluxe, Pinball Dreams, SmashGP, Tears) sat on DATA
  LOADING. The libretro core and bench now classify the card's FAT layout
  (paths only, no file data) and start such cards through the direct loader
  with the card mounted; require_bios keeps the BIOS path. Commercial cards
  are frame-identical on all three boot settings; the five cards match the
  pristine direct boot frame by frame (Pinball Dreams still presents one
  frame on direct boot, a separate open issue). Evidence
  `F:/GP32/results/round159/w-gpmm/` (verify.py, verify-r161.log).
- Round 160 chunked two-region A64 arena: the arena is split into 512 KiB
  chunks; hot bodies grow up from a chunk's start, cold chains down from its
  end, so consecutive hot bodies share lines and the prefetcher runs into the
  next hot body. Blocks are emitted unchanged into the scratch buffer and
  only branches crossing the hot/cold boundary are re-encoded at placement
  (guards keep single B.cond/CBZ/CBNZ forms; the chunk keeps them in range).
  Output identical; H700 ABBA LGM 74.13 -> 76.37 (+3.0%), Blue 84.82 ->
  84.28 (-0.6%), Princess 116.50 -> 116.07 (-0.4%). Native jit/recycle/poll/
  exception/state/callback tests pass on H700.
- Round 159 dense A64 emission: no frame pointer, shape-specific frames,
  forwarding the w2 operand, rotated-immediate logical ops, one load when
  both operands name the same guest register, in-place self-loop budget.
  Output identical; H700 ABBA LGM 71.81 -> 74.18 (+3.3%), Blue 83.78 ->
  84.82, Princess 117.79 -> 116.51 (-1.1%, kept: LGM is the bottleneck).
- Rejected (round 159): retrying soft poll refusals (TLB mapping miss,
  interrupt/epoch cut) instead of pinning a counted poll block to native.
  Recovered 1.6M skipped instructions in Princess slot 0 with identical
  output, but H700 ABBA lost Princess 117.49 -> 114.15 (-2.8%), LGM 71.87 ->
  71.54, Blue 84.08 -> 83.83: the extra dispatcher bookkeeping costs more than
  the skips. A library audit found no other refused MMIO poll worth skipping
  (PWM/UART/IIS/DMA/RTC/ADC status is polled by none of 27 titles). Evidence
  `F:/GP32/results/round158/w-pollaudit/`, `F:/GP32/results/round159/abba-soft/`.
- `4043416`: SMFS card-library wrappers (Samsung/Mirko gp_smc.a, card gate
  SWI 0x11 + closed FAT driver) are fingerprinted structurally in direct
  FPK/FXE mode and routed to the asset-backed file HLE. WinterSports Eins
  (both versions) and DynaMate v1/v2 now render instead of a black screen;
  non-SMFS titles byte-identical; commercial SMC scenes identical.
- Library census at `0f4b15f` (round 159, `F:/GP32/results/round159/w-libsweep/`):
  40 unique SMC cards x {auto input, START script} x {JIT, interpreter},
  3600 frames from BIOS boot: no crashes, JIT == interpreter in 80/80
  configurations at frames 1200 and 3600. Five TOSEC freeware cards with the
  GPMM/ layout (Dynamate, GlooP Deluxe, Pinball Dreams, SmashGP v0.4c, Tears)
  stay on the BIOS "DATA LOADING" screen, as on hardware without the Free
  Launcher; they run when loaded directly. LGM remains the heaviest card
  (~582k native guest instructions and ~64.6k block calls per frame).
- Round 158-159 (installed `308ebb5`, core sha256 `eaae9686…`):
  `526a937` LCD observations once per run window and IO dispatch by
  addr>>20 (LGM +1.1%); `4a00d54` LCDCON5 certified as a run-invariant poll
  read, so the BIOS VSTATUS wait at 0x2740 is skipped exactly (LGM boot
  window LCDCON5 reads 14.6M -> 0.32M); `308ebb5` A64 TLB EOR fold and pair
  self-loop fence (hot bytes -6..-9%; H700 ABBA LGM 70.43 -> 72.04, Blue
  81.80 -> 83.97, Princess 116.52 -> 115.77). H700 frame times at `308ebb5`:
  LGM 71.6 fps p50 21.2 / p99 22.3 / max 25.8 ms (712/1200 over 16.67, none
  over 33.3), LGM boot window 75.7 p99 22.5, Princess 115.6 p99 9.3, ASR
  208.3 p99 13.6 (3 over), Her 182.9 p99 6.6, Blue 83.6 p99 20.6 (5 over).
  Since round 153: LGM 60.7 -> 71.6 (+18%), Blue 75.0 -> 83.6, Princess
  111.9 -> 115.6, ASR 198.6 -> 208.3, Her 172.5 -> 182.9 fps.
- `3a5362a` (round 157): A64 shared block epilogue, cold LDM/STM page retry,
  single 64-bit TLB pair load; hot bytes/block -21..-28%. H700 ABBA identical
  output: LGM 67.16 -> 67.65, Blue 80.35 -> 81.50, Princess 113.90 -> 115.76.
  `e42f656`: same hot/cold layout in the x64 emitter (PC +1-3%, identical).
- Deferred (round 157): audio-buffer-status-driven presentation skip (skip the
  panel decode/staging of a frame while still emulating it exactly). Audio
  output is already exact: 735 frames per retro_run, correct av_info. Skipping
  saves only 2.5-4.7% of a frame (PC) and cannot rescue LGM's 22 ms heavy
  frames, against a public API change and ~1000 lines. Evidence
  `F:/GP32/results/round157/w-audio/`.
- `8d781e0` (round 156): A64 hot/cold block layout. Cold sequences
  (live-read and identity-IO probes, checked helper calls, block-transfer
  second-page paths) are emitted after the block epilogue; fast paths run
  straight through and touch 28-38% fewer 64-byte lines. LGM L1I refills
  1.228e9 -> 0.882e9 (-28%), cycles 58.3e9 -> 53.9e9. H700 ABBA 1.512 GHz,
  identical output: LGM 60.82 -> 67.33 (+10.7%), Blue 75.68 -> 80.50 (+6.4%),
  Princess 110.26 -> 114.20 (+3.6%). The first version double-committed
  single-register PUSH/POP (count==1 fell into a dead two-page path); caught
  by native arm_jit_test, reproduced in Unicorn (`round156/w-cold/uc2`).
- Rejected (round 156): compact C dispatch loop (`arm920t_run` 2224 -> 832
  bytes, rare paths moved to cold functions). On top of `8d781e0` it retired
  +0.7% instructions with unchanged L1I refills and lost ~2% (67.2 -> 65.9).
  Evidence `F:/GP32/results/round156/w-disp/`.
- Round 157 H700 frame times at `607480c` (installed, core sha256
  `871243e0…`): LGM 65.7 fps p50 22.1 / p99 23.2 ms (still 715/1200 over
  16.67 ms; heavy frames need ~1.33x more), Blue 77.9 p99 21.5 (6 over),
  ASR 194.5 p99 15.2 (5 over), Princess 108.8 p99 9.6, Her 173.1 p99 7.1.
- Rejected (round 157): `-ffunction-sections` plus an lld symbol ordering
  file placing the hot C helpers next to `arm920t_run`. L1I refills fell 7%
  (0.957e9 -> 0.891e9) but fps moved only 66.78 -> 66.95 (+0.3%); the zig
  driver also rejects `--symbol-ordering-file`, so it would need a custom link
  step. Evidence `F:/GP32/results/round157/` (order.txt, link.py).
- H700 PMU (round 156, `perf_event_open` user-only, LGM 2400+1200 frames,
  HEAD): cpu_cycles 58.57e9, inst_retired 44.49e9 (IPC 0.76), L1I refills
  1.253e9 (one per 35 instructions), branch mispredicts 0.18e9, L1D refills
  0.036e9, L2 refills 0.011e9. The A53 is I-cache bound, which explains why
  every change that added emitted code lost 4-13% (tail linking raised L1I
  refills +19%). L1I-refill PC sampling: 60% of refills in JIT code (2492
  distinct 64-byte lines; the hottest 1024 lines = 64 KiB cover 95%), 40% in
  C code (`arm920t_run` dispatcher 21.7% of all refills, A64 IO helper 5.4%,
  `io_write32` 4.4%). -O2 equals -O3; -Os is 2.9% slower. Next levers: hot/cold
  splitting of emitted blocks and a compact C dispatch loop. Tools:
  `F:/GP32/results/round156/pmu/` (pmu.c counter wrapper, isamp.c refill
  sampler, run.py, irun.py, jitdist.py).
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
- Rejected (round 154): predicted-successor slot for register-derived exits
  (LDR/LDM/MOV pc, BX) hopping straight into the next native block from the
  chain stub, validated by target PC plus a monotonic code version. On PC it
  halved dispatcher entries for LGM (81.1M -> 40.0M, 97.7% hits) but was not
  faster; native H700 tests passed after fixing an A64 retired-count bug, yet
  H700 ABBA lost LGM 60.87 -> 55.04, Blue 75.66 -> 69.31, Princess 110.44 ->
  106.03 fps. Nested native calls deepen the call stack and the A53 return
  predictor plus stub guards cost more than the C dispatcher round trip.
  Together with the deferred-superblock result this argues against further
  dispatch-stub work on A53. Evidence `F:/GP32/results/round153/w-indirect/`,
  `F:/GP32/results/round154/abba-ic/`.
- `0237a55` (PC only): x64 JIT gained the A64 live-word and identity-IO
  word paths; LGM checked helpers 28.9M -> 1.2M per 1200 frames, PC LGM
  +6-7%. Installed on H700 with the homebrew IRQ work (core sha256
  `3f233960…`); the A64 JIT is unchanged there.
- Rejected (round 155): GPIO-store and LCD-read specialised entries in the A64
  IO word helper with a reduced continuation check (path 179/173 -> 150/84
  instructions). H700 ABBA, identical output: LGM 60.80 -> 61.14 (+0.6%), Blue
  75.70 -> 76.11, Princess 110.65 -> 110.11. Too small for two new bus entries
  and a second continuation predicate. Per-access instruction trimming on the
  A53 has now been tried three ways (identity IO, LCDCON5, GPIO/LCD entries)
  with gains at or under 1%; the IO cost is dominated by the native-to-C
  transition itself. Evidence `F:/GP32/results/round155/w-a64io/`,
  `F:/GP32/results/round155/abba-io2/`.
- Rejected (round 155): direct block linking with tail jumps (four link
  slots per block, lazy fill from the exit fallback, guards re-checked in the
  exit stub, A64 `br` into the successor body). PC: native block calls LGM
  -42%, Blue -84%, identical output, x64 ctest pass. The first A64 build
  crashed on device (leaf blocks do not save x30 and the fill `blr` clobbered
  it; found with Unicorn, fixed). After the fix all native H700 tests passed,
  but ABBA lost LGM 60.91 -> 52.96, Blue 75.46 -> 67.37, Princess 110.87 ->
  100.80 fps. H700 SIGPROF: `arm_jit_link_fill` alone is 2.1% and JIT code
  grew from ~50% to ~53%, so slots are refilled repeatedly (static successor
  PCs that miss guards, e.g. budget/IRQ, fall back every time) and the per-exit
  guard sequence costs more than the C fast path it replaces. Three
  dispatch-avoidance designs (predicted successor, deferred superblock, tail
  linking) all lost 4-13% on the A53. Evidence
  `F:/GP32/results/round155/w-link2/`, `F:/GP32/results/round155/abba-link/`,
  `F:/GP32/results/round155/sampler-link/`.
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
