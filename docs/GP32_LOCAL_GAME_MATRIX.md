# GP32 local game matrix (owned files, deduplicated)

Generated 2026-10-03 (resume12). Files owned by this pass:
docs/GP32_LOCAL_GAME_MATRIX.md and scripts/bench_catalog.py. The earlier
inventory worker failed before writing, so the list below comes from one scripted
pass; no separate manual hash scan was run.

## Scope and method

- Roots: top-level F:/GP32 (one level) and F:/GP32/test-assets (recursive).
- Extensions: .smc, .fxe, .fpk, .zip.
- Dedup key: SHA-256 of the ROM payload. For a plain ROM that is the file itself;
  for a .zip it is the single .smc/.fxe/.fpk entry streamed in memory. A zip and
  an extracted smc of the same game therefore collapse into one ROM.
- Safety: archives are only read. Absolute or traversal entry names are refused,
  at most one ROM entry per archive is auto-selected, and extraction happens only
  when --execute needs it, into a unique folder under
  F:/GP32/results/resume12-catalog-extract/. Caps: 128 MiB per entry, 200:1
  declared compression ratio, 120 s per-file stream budget, 600 s per benchmark
  run, 3600 s per pass (raise with --total-timeout). No ROM was downloaded, no
  asset was modified, and no device or SSH access was used.

## Counts

| Metric | Count |
| --- | --- |
| files scanned | 41 |
| status ok | 34 |
| status no-rom-entry (BIOS zips) | 7 |
| unique ROM payloads | 28 |
| duplicate copies removed by dedup | 6 |

## Unique ROMs

| # | Primary file | Bytes | Payload SHA-256 (16) | Container |
| --- | --- | --- | --- | --- |
| 1 | Astonishia Story R (Korea).smc | 34604032 | 89903d96d17d9895 | rom |
| 2 | Dooly Soccer 2002 (Korea).zip | 17302528 | 4a0000730d89e419 | zip |
| 3 | Dungeon & Guarder - Dragon Gore (Korea).zip | 17302528 | 883116b6ec1e86bf | zip |
| 4 | Dyhard - With Infinite Stairs (Europe).zip | 17302528 | d56bf2338976caec | zip |
| 5 | Dyhard - With Infinite Stairs (Korea).zip | 17302528 | 6d68061676ddc7ae | zip |
| 6 | GP Fight (Korea).zip | 17302528 | a72ba21a0433baa7 | zip |
| 7 | Hany Party Game (Korea).zip | 17302528 | 73da9b61c5765040 | zip |
| 8 | Her Knights - All for Princess - Deadline (Korea).smc | 17302528 | b4c14265f79910d4 | rom |
| 9 | Kimchiman GP32 (Korea).zip | 17302528 | 0b32af64ab81ce84 | zip |
| 10 | Little Girl Mill of a Gingko, The (Korea).zip | 17302528 | 7793488564167e7f | zip |
| 11 | Little Wizard (Korea).zip | 17302528 | 7864ba3c787c601b | zip |
| 12 | OneShot Voca (Korea).zip | 17302528 | 40a4fc63c7b3a59a | zip |
| 13 | Princess Maker 2 (Korea).zip | 17302528 | ab0defbb95d1e208 | zip |
| 14 | Rally Pop (Korea).zip | 17302528 | 1bc8f698997d7e08 | zip |
| 15 | Raphael (Korea).zip | 17302528 | fef67e4ef302a872 | zip |
| 16 | Tanggle's Magic Square (Korea).zip | 17302528 | b6b68c878e32190a | zip |
| 17 | Blue Angelo - Angels from the Shrine (Europe) (En,Fr,De,Es,It,Pt).smc | 17302528 | 87a01bd29f98004b | rom |
| 18 | Dungeon & Guarder - Dragon Gore (Europe).smc | 17302528 | f4b4e4404e6fe49d | rom |
| 19 | Her Knights - All for the Princess (Europe).smc | 17302528 | 1bfb26b796c00fa8 | rom |
| 20 | Little Wizard (Europe).smc | 17302528 | 4de9fb58aedcb140 | rom |
| 21 | Therapy (Korea).zip | 17302528 | 82edd4e41e64520b | zip |
| 22 | Tomak - Save the Earth, Again (Europe) (v1.0).zip | 17302528 | 51bf9b146b02b2e2 | zip |
| 23 | Tomak - Save the Earth, Again (Europe) (v2.0).zip | 17302528 | 4dc439666f65ff1c | zip |
| 24 | Tomak - Save the Earth, Again (Korea).zip | 17302528 | 36c1b19b95ff840e | zip |
| 25 | W.B.W. - Wanna Be Wizard! (Korea).zip | 17302528 | 02fc502d7b70a5e5 | zip |
| 26 | Wizard Slayer (Korea).zip | 17302528 | 13a3ecc22d69d3ac | zip |
| 27 | Woody & Kunta - Treasure Island (Europe).zip | 17302528 | fdc559b48cbe471d | zip |
| 28 | Woody & Kunta - Treasure Island (Korea).zip | 17302528 | 06daf862483e86ba | zip |

Full hashes, CRCs and per-file status are in
F:/GP32/results/resume12-catalog-inventory.json (companion -inventory.md).

## Duplicate groups (same payload, different container)

| Payload SHA-256 (16) | Copies | Members |
| --- | --- | --- |
| 89903d96d17d9895 | 2 | Astonishia Story R (Korea).smc + Astonishia Story R (Korea).zip |
| b4c14265f79910d4 | 2 | Her Knights - All for Princess - Deadline (Korea).smc + Her Knights - All for Princess - Deadline (Korea).zip |
| 87a01bd29f98004b | 2 | Blue Angelo - Angels from the Shrine (Europe) (En,Fr,De,Es,It,Pt).smc + .zip |
| f4b4e4404e6fe49d | 2 | Dungeon & Guarder - Dragon Gore (Europe).smc + Dungeon & Guarder - Dragon Gore (Europe).zip |
| 1bfb26b796c00fa8 | 2 | Her Knights - All for the Princess (Europe).smc + Her Knights - All for the Princess (Europe).zip |
| 4de9fb58aedcb140 | 2 | Little Wizard (Europe).smc + Little Wizard (Europe).zip |

## Catalog coverage against docs/GP32_GAME_CATALOG.md

- 21 of the 31 MAME titles have a local payload. The 28 payloads include the
  Korean and European variants of Dungeon & Guarder, Dyhard, Her Knights, Little
  Wizard and Treasure Island, plus all three Tomak variants.
- Not present locally (10 titles): Funny Soccer 2002 (funnysoc), GlooP Deluxe
  (gloopdx), Holeman Battle Race 2002 (holbatra), Pinball Dreams (pinbdrea),
  Story of Bug eyed Monster (sobemons), Winter Is (winteris), Super Plusha
  (suplusha), Tales of Windy Land (talowila), Topy Topy Gogo (totogogo),
  Tears - Another Story (tearsast). No download was attempted for these.

## Benchmark runner (frozen)

| Item | Value |
| --- | --- |
| Runner | F:/GP32/results/resume10-input-fixed-bench.exe |
| Runner SHA-256 | 8c84c81802f4f1e1b9750efedffa1075f4aba07c17fcc79c59e503dddb57fccf |
| BIOS | F:/GP32/test-assets/[BIOS] GamePark GP32 (Europe) (v1.6.6).bin |
| BIOS SHA-256 | ECF63B73BBD13668F9FEF259EE276BD14A81B0BB420FFF1678613E7ED7C814F7 |
| Flags | --bios --smc --warmup 2400 --frames 300 --jit (autopulse input, no state) |

Warning: this window is a boot/menu/attract window. Every record carries
scene_kind=boot-menu-300f, gameplay_measured=false and manual_gameplay_pending=true.
A metric here is not gameplay and not a pass/fail gate.

## Smoke run (one game, pre-portability revision)

| Field | Value |
| --- | --- |
| ROM | OneShot Voca (Korea), payload 40a4fc63c7b3a59a |
| Status | metric |
| fps | 2075.811 |
| elapsed | 0.144522 s |
| cycles | 1525735000 |
| pc / cpsr | 0x0c7b154c / 0x20000053 |
| clock | 33900000 |
| audio_frames | 0 |
| video_hash | 1783222f0e3f7795 |
| exit code | 0 |
| Extracted payload | F:/GP32/results/resume12-catalog-extract/40a4fc63c7b3a59a/OneShot Voca (Korea).smc |

Evidence: F:/GP32/results/resume12-catalog-bench.json and -bench.md.

## Batch result (Windows, frozen pre-portability revision)

The parent ran the full batch with the frozen pre-portability revision of this
script and resume10-input-fixed-bench.exe (SHA-256 8c84c818...). Result file:
F:/GP32/results/resume12-all-games-bench.json.

- 28 of 28 selected ROMs produced a metric; 0 errors, 0 timeouts.
- Every row: Windows host, 2400 warmup + 300 measured frames, --jit, autopulse
  input, scene_kind boot-menu-300f. This is a boot/menu/attract window, not
  gameplay and not a pass/fail gate, and the rows are not a speed ranking: each
  game is in a different screen during those 300 frames.

| # | ROM | Status | fps | Exit | Duration s |
| --- | --- | --- | --- | --- | --- |
| 1 | Astonishia Story R (Korea).smc | metric | 997.689 | 0 | 2.006 |
| 2 | Dooly Soccer 2002 (Korea).zip | metric | 506.592 | 0 | 2.236 |
| 3 | Dungeon & Guarder - Dragon Gore (Korea).zip | metric | 1693.709 | 0 | 1.845 |
| 4 | Dyhard - With Infinite Stairs (Europe).zip | metric | 1185.001 | 0 | 1.659 |
| 5 | Dyhard - With Infinite Stairs (Korea).zip | metric | 1173.066 | 0 | 2.125 |
| 6 | GP Fight (Korea).zip | metric | 1412.431 | 0 | 1.677 |
| 7 | Hany Party Game (Korea).zip | metric | 1870.7 | 0 | 1.591 |
| 8 | Her Knights - All for Princess - Deadline (Korea).smc | metric | 758.119 | 0 | 2.055 |
| 9 | Kimchiman GP32 (Korea).zip | metric | 1246.58 | 0 | 3.237 |
| 10 | Little Girl Mill of a Gingko, The (Korea).zip | metric | 246.726 | 0 | 3.384 |
| 11 | Little Wizard (Korea).zip | metric | 513.116 | 0 | 2.609 |
| 12 | OneShot Voca (Korea).zip | metric | 2388.889 | 0 | 1.167 |
| 13 | Princess Maker 2 (Korea).zip | metric | 2169.312 | 0 | 1.029 |
| 14 | Rally Pop (Korea).zip | metric | 1411.68 | 0 | 1.316 |
| 15 | Raphael (Korea).zip | metric | 1495.953 | 0 | 1.742 |
| 16 | Tanggle's Magic Square (Korea).zip | metric | 332.429 | 0 | 5.059 |
| 17 | Blue Angelo - Angels from the Shrine (Europe) (En,Fr,De,Es,It,Pt).smc | metric | 964.375 | 0 | 2.037 |
| 18 | Dungeon & Guarder - Dragon Gore (Europe).smc | metric | 1655.425 | 0 | 1.933 |
| 19 | Her Knights - All for the Princess (Europe).smc | metric | 583.569 | 0 | 2.62 |
| 20 | Little Wizard (Europe).smc | metric | 384.036 | 0 | 2.918 |
| 21 | Therapy (Korea).zip | metric | 945.753 | 0 | 1.37 |
| 22 | Tomak - Save the Earth, Again (Europe) (v1.0).zip | metric | 2641.722 | 0 | 1.301 |
| 23 | Tomak - Save the Earth, Again (Europe) (v2.0).zip | metric | 483.599 | 0 | 1.782 |
| 24 | Tomak - Save the Earth, Again (Korea).zip | metric | 416.626 | 0 | 2.833 |
| 25 | W.B.W. - Wanna Be Wizard! (Korea).zip | metric | 1694.734 | 0 | 1.075 |
| 26 | Wizard Slayer (Korea).zip | metric | 1973.913 | 0 | 1.255 |
| 27 | Woody & Kunta - Treasure Island (Europe).zip | metric | 668.243 | 0 | 1.7 |
| 28 | Woody & Kunta - Treasure Island (Korea).zip | metric | 841.045 | 0 | 1.462 |

Observed spread in this boot/menu window only: 246.726 fps (Little Girl Mill of a
Gingko, Korea) to 2641.722 fps (Tomak, Europe v1.0). Recorded to describe the
window, not to rank titles; gameplay fps for these titles remains unmeasured.

## Re-running (portable defaults)

Defaults now derive from the script location (Path(__file__).resolve()): the
repository parent directory is the default asset root, its test-assets/
subdirectory is scanned recursively, and results/ under the same parent is the
default output directory. --execute requires explicit --benchmark and --bios.

python scripts/bench_catalog.py --execute --benchmark <runner.exe> --bios <bios.bin> --total-timeout 14400

Targeted gameplay runs (one at a time; --state requires --limit 1):

python scripts/bench_catalog.py --execute --only "Her Knights - All for Princess - Deadline" --limit 1 --state <her-combat.state> --input-script <her-combat.txt> --benchmark <runner.exe> --bios <bios.bin>

python scripts/bench_catalog.py --execute --only "Little Wizard (Europe)" --limit 1 --state <wizard-combat-entry.state> --benchmark <runner.exe> --bios <bios.bin>

Machine-specific paths for this workspace stay in the runner table above.

## Measured gameplay reference (retained lazy epoch)

| Title | Scene | Core fps |
| --- | --- | --- |
| Her Knights (Korea) | first palace battle, 1200/1200 | 90.5365 |
| Little Wizard (Korea) | character selection, 2400/300 | 50.493 |
| Little Wizard (Europe) | real combat entry (state at frame 3350) | not yet measured |
| Blue Angelo (Europe) | extracted gameplay, 1200/300 | 66.806 |

## resume13 gameplay evidence (frozen 006fab9 runner, states + scripts)

Extended 2026-10-03 (resume13). Runner: F:/GP32/results/resume13-headless.exe
(006fab9 core, source commit 006fab97522edc69a68e2851e30c14d3e234c4de, plus the
--save-state option), SHA-256 aa29d3f92d394f77db0cf82b10317ebfd6ff3e2fe905a61e1aeda4e894bd171f.
BIOS unchanged: F:/GP32/test-assets/[BIOS] GamePark GP32 (Europe) (v1.6.6).bin,
SHA-256 ECF63B73BBD13668F9FEF259EE276BD14A81B0BB420FFF1678613E7ED7C814F7.
Each row below describes a bounded replay or state capture with this frozen
runner from an owned payload. A state is paired
with its matching ROM payload and must not be reused with another title.

Boundaries: these are verification runs, not speed measurements (no stable-FPS
claim and no ranking), not a compatibility pass for any title, and no physical
audio verification was performed.

| Title (payload SHA-256 16) | Verified scene | State label | Script label | Run end cycles / PC | Screenshot label |
| --- | --- | --- | --- | --- | --- |
| Little Girl Mill of a Gingko, The (Korea) 7793488564167e7f | stage-1 entry gate is NOT gameplay (wait screen) | resume13-mil-gameplay.state, sha256 ec81ccbbd75c0d85... | resume13-mil-gameplay.txt | 9830858333 / 0x0c7b1548 | resume13-mil-gameplay.png |
| Little Girl Mill of a Gingko, The (Korea) 7793488564167e7f | in-room controllable stage after the LOADING transition; held directional input moves the sprite | resume13-mil-stage.state, sha256 523eb8c4ddf647ca... -> resume13-mil-stage2.state, sha256 7fb5f3684deb7174... | resume13-mil-stage.txt (RIGHT hold), resume13-mil-stage2.txt (LEFT 0..300, RIGHT 420..900) | 14915858333 / 0x0c7b1548 | resume13-mil-stage2-left240.png, resume13-mil-stage2-right780.png |
| Tomak - Save the Earth, Again (Korea) 36c1b19b95ff840e | real shooting verified at dynamic 2400 default pacing | resume13-tomak-gameplay.state | resume13-tomak-shoot-2400.txt | 7980000000 / 0x0c016af4 | resume13-tomak-dynamic-end.png, sha256 84869a90a5315dec... |
| Little Wizard (Korea) 7864ba3c787c601b | actual battle, 2400 frames, in-game timer 85 -> 39 | resume13-wizard-korea-combat.state, sha256 5d56b6d938fb19fb... | resume13-wizard-combat.txt, sha256 7c65c973ba609f60... | n/a | resume13-wizard-korea-combat.png |

### Little Girl Mill of a Gingko, The (Korea): entry gate vs in-room stage

The stage-1 entry gate is a wait screen, not gameplay: with no input the frame
stayed byte-identical over 1400 captured frames (9200 -> 10600) and over 300
more frames after a state reload (0 differing pixels of 76800). With RIGHT/A
input the same state starts a LOADING transition (parent replay, --frames 300:
cycles 10113358333, PC 0x0c077a84); the in-room stage appears 600-900 frames
later. So "identical frames over N frames" is not proof of gameplay for this
title.

Movement evidence (fixed camera, 54-px sprite): with LEFT held from frame 0 the
sprite translated 113 px left by frame 120 and 127 px by frame 240, then settled
at the wall; with RIGHT held from frame 420 the same 127 px travel went back and
stopped at the tree. HUD changes across the transition: X 0 at the gate, X 10
in the stage.

Reproducible commands (labels relative to the repository parent; both runs add
`--bios "test-assets/[BIOS] GamePark GP32 (Europe) (v1.6.6).bin" --jit --rotate-ccw`):

- exit the gate: results/resume13-headless.exe --smc results/resume13-mil-rom.smc --load-state results/resume13-mil-gameplay.state --input-script results/resume13-mil-stage.txt --frames 4200 --save-state results/resume13-mil-stage.state
- movement proof: results/resume13-headless.exe --smc results/resume13-mil-rom.smc --load-state results/resume13-mil-stage.state --input-script results/resume13-mil-stage2.txt --frames 1200 --save-state results/resume13-mil-stage2.state

Script grammar: FRAMEf:=BUTTON sets the held state from that frame, FRAMEf:P
taps START for one frame. Full per-frame timelines, cluster measurements and
hashes: F:/GP32/results/resume13-mil-log.md.


## Open items

- The batch is complete (batch result section above). The integrated resume12
  Her Knights Korea battle comparison passes with exact CPU/video/PCM at matched
  1.512 GHz: 90.098 -> 101.2165 core fps (+12.34%). Little Wizard combat-entry
  remains unmeasured on H700. Its European state must not be combined with the
  newly installed Korean ROM; save states require the matching content.
- Homebrew and app enumeration is outside these asset roots (catalog doc section 4).
- Ten catalog titles without a local payload stay hypothesis-only until an owned
  copy exists; no ROM download will be attempted.
