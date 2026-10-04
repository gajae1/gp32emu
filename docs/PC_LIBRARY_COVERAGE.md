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

Summary: 26 reached gameplay/story/application, two menu/setup only, five loading-stalled (including two homebrews). Winter Is story progression is not established merely by selecting Start. Funny Soccer reached character hub/shop but no match.

Source SMC hashes, exact inputs, states and screenshots are in the private `resume83-library` manifest/coverage and per-game run directories. No ROM, BIOS, state or screenshots are committed. Parallel probe/profile timings are not performance acceptance.

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
installed on H700 together with counted-poll acceleration (current source
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
H700 gameplay acceptance test. No ROM/BIOS modifications or audio playback were
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
