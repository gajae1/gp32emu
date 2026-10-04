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
