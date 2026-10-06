# PC library coverage - 2026-10-04

Frozen core baseline: 2341f6c (before GPIO cache/audio delivery changes). 33 cold starts; screenshots and explicit continuation input retained in private evidence. Gameplay below means the visible scene was reached, not full-game compatibility or hardware performance acceptance.

| ID | Title | Deepest observed scene |
|---:|---|---|
| 1 | Astonishia Story R (Korea) | In-game story/stage dialogue |
| 2 | Blue Angelo - Angels from the Shrine (Europe) (En,Fr,De,Es,It,Pt) | Gameplay/home hub/board |
| 3 | Dooly Soccer 2002 (Korea) | Gameplay/home hub/board |
| 4 | Dungeon & Guarder - Dragon Gore (Korea) | In-game story/stage dialogue |
| 5 | Dyhard - With Infinite Stairs (Korea) | Gameplay/home hub/board |
| 6 | Funny Soccer 2002 (Korea) | Menu/setup; play not verified |
| 7 | GP Fight (Korea) | Gameplay/home hub/board |
| 8 | GlooP Deluxe (Europe) | BIOS loading stalled |
| 9 | Hany Party Game (Korea) | Gameplay/home hub/board |
| 10 | Her Knights - All for Princess - Deadline (Korea) | Gameplay/home hub/board |
| 11 | Holeman Battle Race 2002 (Korea) | Gameplay/home hub/board |
| 12 | Kimchiman GP32 (Korea) | Gameplay/home hub/board |
| 13 | Little Girl Mill of a Gingko, The (Korea) | Gameplay/home hub/board |
| 14 | Little Wizard (Korea) | Gameplay/home hub/board |
| 15 | OneShot Voca (Korea) | Interactive vocabulary application |
| 16 | Pinball Dreams (Europe) | BIOS loading stalled |
| 17 | Princess Maker 2 (Korea) | Gameplay/home hub/board |
| 18 | Rally Pop (Korea) | Gameplay/home hub/board |
| 19 | Raphael (Korea) | Gameplay/home hub/board |
| 20 | Story of Bug Eyed Monster (Korea) | In-game story/stage dialogue |
| 21 | Super Plusha (Europe) | Gameplay/home hub/board |
| 22 | Tales of Windy Land (Korea) | Gameplay/home hub/board |
| 23 | Tanggle's Magic Square (Korea) | Gameplay/home hub/board |
| 24 | Tears - Another Story (Korea) | BIOS loading stalled |
| 25 | Therapy (Korea) | In-game story/stage dialogue |
| 26 | Tomak - Save the Earth, Again (Korea) | Gameplay/home hub/board |
| 27 | Topy Topy Gogo (Europe) | Gameplay/home hub/board |
| 28 | W.B.W. - Wanna Be Wizard_ (Korea) | Gameplay/home hub/board |
| 29 | Winter Is (Korea) | Menu/setup; play not verified |
| 30 | Wizard Slayer (Korea) | Gameplay/home hub/board |
| 31 | Woody & Kunta - Treasure Island (Korea) | Gameplay/home hub/board |
| 32 | Dynamate v2.0 (2003-03-25)(Flavor)(FW) | BIOS loading stalled |
| 33 | SmashGP v0.4c (2005-08-11)(mATkEUpON)(FR)(FW) | BIOS loading stalled |

Baseline summary: 26 reached gameplay/story/application, two menu/setup only, five loading-stalled (including two homebrews). The dated follow-ups below supersede the individual baseline limitations where stated.

Source SMC hashes, exact inputs, states and screenshots are in the private `resume83-library` manifest/coverage and per-game run directories. No ROM, BIOS, state or screenshots are committed. Parallel probe/profile timings are not performance acceptance.

## Funny Soccer match follow-up (core `6cb7d8c`)

Funny Soccer 2002 Korea now reaches a match from its saved hub/shop state with
the original BIOS and SMC. In the shop, three DOWN presses move from the first
item through the second row and item belt to Exit. A confirms; the following
loading interval must finish before navigating again. From the hub's Item Shop
selection, four DOWN presses select SEOGWIPO, and A starts the match after
another loading interval. No game-specific emulator change was needed.

The 1,200-frame continuation captures the field, opposing teams, HUD and a
0:1 score. Matched 120-frame replays from that state compare LEFT held for
90 frames with no input: player positions differ visibly, proving directional
control rather than merely an attract scene. Both replays produce 44,112
source PCM frames with identical PCM hashes. This is a short gameplay/input
and digital-output check, not a complete match, audio-listening or handheld
performance verdict. It does not replace the earlier cold-start evidence.

A fresh 300-frame PC profile records 100 CP15 cache-maintenance events,
7,517 translated blocks, and no code-arena recycle. Those maintenance counts
do not mean 100 full cache flushes: the existing cache-epoch path lazily checks
recorded instruction bytes and retains unchanged blocks. The profile alone
does not justify changing invalidation semantics.

Evidence: private `resume101-coverage/funny-result.json`, `funny-profile.json`,
`funny-match/`, and the matched `funny-control-{left,idle}/` captures. Intermediate
generated state files can be reconstructed from the retained input chain;
the original user state and final match state are preserved.

## Winter Is story follow-up (core `6cb7d8c`)

Three bounded 600-frame PC replays resolve the earlier menu-only observation.
From the saved NOVEL start/continue submenu, A dismisses the submenu, whereas
B confirms Start. Subsequent B presses advance narration into the night
train-station scene and the chapter card "여행 첫날." No emulator change or
per-title input remapping was necessary; the earlier input sequence assumed
the wrong confirm button.

This verifies story entry and text advancement from the saved menu state,
not full-game, cold-boot, handheld or sound compatibility. The final continuation
produces zero source PCM frames; reaching a visible story scene alone does
not establish whether its silence matches original hardware.

Evidence: private `resume101-coverage/winter/REPORT.md`, `SHA256.txt`, and
`run-{1,2,3}/` commands, inputs, logs and captures. The final chapter-card
state is retained; intermediate generated states and duplicate PPMs were
removed after preserving PNG captures.

Together with the firmware and Pinball follow-ups below, all 31 commercial
families now have evidence beyond the initial menu/loading screen (some only
level selection or a playfield). These observations span different core
revisions, firmware versions and saved-state continuations. They are not a
single-build compatibility pass or proof that every title is fully playable.

## Firmware follow-up

The table above retains the original BIOS baseline (SHA-256 prefix
`ecf63b73bbd1`). A subsequent unchanged-core/unchanged-SMC cold-start comparison
used supplied official v1.6.6 images and 3,600 frames per run:

| Title | Beta `c9a09a1be3fd` | 2003-05-21 `797d9ba36794` | 2004-10-10 `ba237195539f` |
| --- | --- | --- | --- |
| GlooP Deluxe | Gameplay/HUD | Level selection | Level selection |
| Tears | Korean story dialogue | Korean story dialogue | Korean story dialogue |
| Pinball Dreams | White screen, no PCM | White screen, no PCM | White screen, no PCM |
| Dynamate | Puzzle board | Puzzle board | Puzzle board |
| SmashGP | Title / Press Start | Title / Press Start | Title / Press Start |

These are firmware-dependent observations, not a new emulator fix or proof
that the original BIOS is faulty. Shared CPU/device behavior may still be
involved. The original GlooP loading loop also reproduces with the interpreter:
independent 2,400-frame cold starts plus ten measured frames match all seven
CPU/video/PCM fields with JIT on/off. This narrows the investigation beyond a
native-JIT-only defect. Do not substitute BIOS files automatically by title.

Private evidence: `resume84-firmware/{inventory,gloop-results,more-results}.json`,
per-run states/screenshots and `resume83-loading-parent/gloop-cold-parity.json`.
Existing homebrew samples remain useful diagnostics; further homebrew game
downloads are outside the user's requested collection scope.

The original BIOS's captured loading loop formats a string from `0x00080000`,
just beyond the mapped 512-KiB image. Reading unmapped `0xff` bytes until the
first RAM zero explains the observed `0x0bf80001` string length. The corrupt
pointer's upstream cause remains unresolved; matching interpreter/JIT behavior
does not exclude a shared CPU or peripheral defect. ROM mirroring was proposed
as a way to shorten this scan, but is not adopted without evidence of actual
address decoding and a correction to the originating bad state. Private trace:
`resume84-bios-diff/FINDINGS.md`.

The subsequent LCD scheduling correction addresses a separate Pinball wait:
its guest loop waits for `LINECNT == 8`, which frame-aligned coarse CPU slices
could miss indefinitely. The development core limits a run at the next visible
line transition only after an actual LCD status read. The captured Pinball state
now reaches a background/copyright screen after 120 frames. This establishes
progress past that wait, not gameplay compatibility. The correction is now
installed on the handheld together with counted-poll acceleration (starting with source
`e153e45`); its scheduling cost and bounded performance results are documented
in `GP32_PERFORMANCE_STRATEGY.md`.
Private evidence: `resume84-lcd/demand-120frames/`, `device-demand.json`, and
`demand-parity.json` (matching PC/AArch64 CPU, video and PCM results in three
existing replay scenes). The current core retains the FIFO restart correction.

A follow-up using the frozen `e153e45` PC headless build reproduces that saved
screen byte-for-byte, generates nonzero PCM and advances through copyright
fade-out to the Logik State logo. From the saved logo state, one 2400-frame
continuation with spaced START/A presses reaches the Ignition table-selection
and high-score screen. A further 240-frame continuation with A pressed at
frames 5-16 reaches the **Ignition playfield**, confirmed by viewing the capture.
This establishes progress into the game and a responding selection input;
ball launch, scoring, flippers, other tables and long-session behavior remain
untested. It is a state-continuation result, not a new complete cold-boot or
handheld gameplay acceptance test. No ROM/BIOS modifications or audio playback were
used. Evidence: `resume90-pinball/FINDINGS.md`,
`parent-next/{command,input-manifest}.json`, and
`parent-next/table-select/{command.json,screen.png,end.state}`.

Further original-BIOS analysis locates a two-directory list builder that tries
both `gp:\\game\\` and `gp:\\gpmm\\`. GlooP's root contains GPMM but no GAME.
The disassembled directory functions return on path-resolution failure without
initializing their count output, while the caller proceeds when either path
succeeds. The captured stack contains a pointer-sized stale count; a live trace
also observes a NULL-buffer directory walk (`r10 == r9 * 16`). This supports a
firmware error-path explanation for the huge string scan. The exact earlier
writer of the stale stack slots and real-hardware behavior remain unverified.

A scratch-only counterfactual added a GAME root entry aliasing GPMM's cluster.
With the same core and 3600-frame input sequence, the original image remains at
DATA LOADING. Initially the altered image reached "SMC is not inserted or no game
exists": that diagnostic had left the changed NAND page's ECC stale. After
recomputing its ECC, the same alias image reaches GlooP's **level-selection
screen**. The original page's ECC matched the reference calculation (with the
SmartMedia byte order), so this controls a confounding media-integrity error.
This supports the missing-directory error path as the trigger. The alias image
is a diagnostic, not a production filesystem repair or evidence of full play.
It does not justify automatically modifying users' images or mirroring ROM.
Original media and device firmware were unchanged.

ECC was calculated in a private diagnostic using the GPL-licensed
[Linux NAND ECC reference](https://raw.githubusercontent.com/torvalds/linux/v2.6.12/drivers/mtd/nand/nand_ecc.c),
retaining its attribution; no reference implementation was added to the emulator.
Private evidence: `resume85-bios-origin/FINDINGS.md` and
`resume86-bios-counterfactual/{result,ecc-result}.json`, with `comparison.png`.

A bounded Tears follow-up supports the same original-BIOS failure path: its
unchanged card also has GPMM but no GAME, and a single 120-frame continuation
from the existing stuck state reaches the same printf/byte-reader pair with
`r7=0x0bf80001`, `r3=0x00080001`, PC `0x0c02671c`, and LR `0x0c027f68`.
The resulting screen is byte-identical to the saved loading screen. Three
previous alternate-firmware runs reached Korean story dialogue. This is strong
evidence of a shared firmware error path, not proof that every loading failure
has this cause: Tears' earlier uninitialized count slot and real-hardware
behavior were not observed. No card or firmware was modified. Evidence:
`resume92-tears-origin/FINDINGS.md`, `tears-fat.json`, and
`run-120/{stdout.txt,provenance.json}`.


## Single-build library refresh (2026-10-05, b31cb2f)

All 31 catalog titles were cold-started for 3600 virtual frames on one frozen
PC profile library. Seven disjoint shards were assigned; five provider calls
failed with 429, and the parent completed their runs locally. Fourteen agent
assignments across runtime, SDK and coverage work included those failed calls;
this is not a claim of fourteen simultaneously productive workers.

Twenty-seven titles also received one 600-1200-frame continuation, using
explicit inputs and either the new boot state or a documented older scene.
All 58 bounded runs exited successfully. Visual inspection, not exit status,
determined the scenes below. Menus, active gameplay, and state continuations
are deliberately separate. No device ROMs, user states, volume or settings
were modified. Timings from parallel profile runs are not speed measurements.

| ID | Title | Current-build observed scene / limit |
| ---: | --- | --- |
| 1 | Astonishia Story R | Title; older-state continuation reaches inn dialogue |
| 2 | Blue Angelo | Language selection; older-state continuation reaches field/HUD |
| 3 | Dooly Soccer | Match dialogue and goal presentation |
| 4 | Dungeon & Guarder | Difficulty menu to field/story with party and HUD |
| 5 | Dyhard | New/load/save menu; no deeper continuation this batch |
| 6 | Funny Soccer | Character selection; prior match state continues on field |
| 7 | GP Fight | Character/story sequence and partially filled match-loading bar; match not reached within cap |
| 8 | GlooP Deluxe | Default BIOS loading screen; beta-BIOS playfield state loads, scripted START opens pause menu |
| 9 | Hany Party Game | Title; prior-state continuation reaches platform minigame/HUD |
| 10 | Her Knights | Title; prior combat state continues in stage |
| 11 | Holeman | Title; prior racing state continues on track/HUD |
| 12 | Kimchiman | Combat sprites/HUD visible in cold boot; controls not attributed |
| 13 | Little Girl Mill | Story; prior-state continuation reaches character/title presentation, no new movement proof |
| 14 | Little Wizard | Character selection to VS and active match/round progression |
| 15 | OneShot Voca | Vocabulary controls and word/definition presentation |
| 16 | Pinball Dreams | Default BIOS loading screen; 2003-05-21 BIOS state continues on Ignition playfield |
| 17 | Princess Maker 2 | Naming screen; prior-state continuation reaches room/status panel |
| 18 | Rally Pop | Level selection to arena/characters/HUD |
| 19 | Raphael | Opening illustration; no deeper continuation this batch |
| 20 | Story of Bug Eyed Monster | Story dialogue; scripted continuation opens save confirmation |
| 21 | Super Plusha | Menu to platform stage, enemies and HUD |
| 22 | Tales of Windy Land | Story; prior-state continuation reaches outdoor field |
| 23 | Tanggle's Magic Square | Puzzle board |
| 24 | Tears | Default BIOS loading screen; beta-BIOS state continues Korean story |
| 25 | Therapy | Title to Korean narration/story |
| 26 | Tomak | Arcade/options menu; no deeper continuation this batch |
| 27 | Topy Topy Gogo | Title; prior racing state continues in race |
| 28 | W.B.W. | Title to forest field and wolf dialogue |
| 29 | Winter Is | Menu; prior chapter state advances Korean train narration; sound fidelity unresolved |
| 30 | Wizard Slayer | Difficulty menu; prior-state continuation reaches dark field/transition, controls not attributed |
| 31 | Woody & Kunta | Menu; bounded prior-state continuation ends at menu, no fresh gameplay claim |

The three default-BIOS loading limitations are reproduced historical findings,
not three new emulator defects. Alternate BIOS plus saved-state continuation
is not equivalent to a successful cold boot with the default BIOS. Likewise,
GP Fight's changing loading presentation at the time cap is not a proven hang.
Old/new PCM hashes differ across intended IIS fractional-clock fixes; a hash
change across those revisions alone does not establish an audio regression.

Evidence: private `resume116-library/manifest.json`, `parent-{boot,extra,
continuation}.json`, per-run commands/metrics/captures, `fallback-state-map.json`,
and `shard-{4,7}/REPORT.md`. The frozen library SHA-256 is
`c90ce3855d88ef3b364d348a390cd911896ffdef6fee723eb91c7cb14059d7f5`;
the probe SHA-256 is
`51dd513d4ba728dd6db3e413a051019da30434b8a0201637584a92103962774c`.
Subsequent portable-op pointer optimization was tested separately and is not
silently substituted for this frozen coverage revision.

New common diagnostic leads are the register-insensitive LCD period/static
LCDCON5 status and Blue partial-frame audio delivery deficit; see the latest
sections of GP32_PERFORMANCE_STRATEGY.md. The user's speed and audible-pop
reports, full gameplay/long sessions, input latency and Android runtime remain
open. Successful boot screenshots do not close those gates.

## TFT candidate follow-up (2026-10-05)

The register-derived TFT candidate repeats all 31 cold boots and three older
ASR/Blue/Princess scenes successfully at the harness level. Two apparent scene
changes received bounded follow-ups: Tales' black final capture advances into
story/title without input, while Super Plusha finishes its opening and reaches
a platform stage with input. Neither observation establishes a hang. Super
Plusha's opening finishes later in the fixed-time captures; original timing
fidelity remains unverified. Details: [LCD_TIMING.md](LCD_TIMING.md).

Proactive triage still prioritizes GlooP/Pinball/Tears' default-BIOS loading
paths and controls not yet attributed in several scenes; GP Fight's match
loading is now observed to complete, leaving the unresolved gray final capture
as its open item (exact follow-up below). Zero-PCM windows in Winter Is, Bug Eyed Monster and other
scenes are investigation leads, not confirmed missing-sound defects: silence
may be intentional. New analysis uses [SMC executable inspection](SMC_ANALYSIS.md)
and SDK/firmware call sites; it does not count an extracted file or static
instruction as proof of runtime correctness.

## GP Fight match-loading follow-up (2026-10-05, resume127-compat)

One 3,600-frame no-input continuation from the saved partially-filled
match-loading state (`resume116-library/shard-7/07-gpfight/cont1/end.state`)
completes loading with the current probe: frame 300 still shows the filling
NOW LOADING bar, frame 600 shows a live four-fighter match with HP bars, and
frame 3300 shows a P1 KO banner. Console windows show per-window video changes
and nonzero PCM through the match. No emulator change produced this progress;
the earlier continuation cap simply ended before the finite wait finished.

The frame-3600 capture is a uniform gray screen whose destination is
unresolved; it is not classified as a fade, idle or post-match transition
without further evidence. This establishes that match loading completes and a
match scene runs, not full-game compatibility, playability, input behavior or
timing fidelity. Exact follow-up: one further short (~300-600f) no-input
continuation from `resume127-compat/gp-fight/run/cont2-loading-3600/end.state`
to identify what the gray screen lands on.

Evidence: `resume127-compat/gp-fight/run/cont2-loading-3600/` console.txt,
png/frame-000600.png, png/frame-003300.png and end.state.

## GP Fight gray-screen destination (2026-10-05, resume130-compat)

A 600-frame no-input continuation from that exact end state reaches GAME OVER
at frame 300 and the title/PUSH ANY BUTTON screen at frame 600. Both captures
were inspected; the gray screen was a progressing post-KO transition in this
bounded sequence. This resolves the destination question above without an
emulator fix, not full-game or input acceptance.

The probe is binary-identical to the preceding resume127 run (SHA-256
`055643f3eca09e9edb7e0f7a07d09cac442762492fc238f2e05ddc6da8410348`).
It predates `a61da73`; its exact source working-tree revision is not embedded
or proven by file timestamps. This is a historical-binary follow-up, not a
current-core regression result. Evidence: private
`resume130-compat/gpfight/report.md` and `run/cont3-gray-600/` captures,
console, command and end state. No further gameplay is claimed.
