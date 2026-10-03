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


## resume16: Mill room-to-village progression

The Korean Mill payload above now has a reproducible 1,700-frame progression:
walk around the tree, leave along the back wall, enter ROOT Village, and
advance Korean NPC dialogue with A. This resolves the earlier room-exit
uncertainty; it does not establish combat or full-game compatibility.

The input sequence below starts from `resume13-mil-stage.state` (SHA-256
`523eb8c4ddf647ca6bfdb7b8bd72a7191011fbfcf197af194920b3f9b646b743`):

```text
30f:=DOWN
200f:=NONE
220f:=RIGHT
600f:=NONE
620f:=UP
860f:=NONE
880f:=LEFT
1300f:=NONE
1400f:A
1440f:A
1480f:A
1520f:A
1560f:A
```

Local evidence: `F:/GP32/results/resume16-mill/README.md`,
`canonical.txt`, `can-d1000.png`, `can-d1450.png`, and `can-village.state`.
States and game content remain outside Git. The measured H700 run and its
limits are recorded in [the performance strategy](GP32_PERFORMANCE_STRATEGY.md).

## resume17: GP Fight Korean match entry

The owned Korean payload `a72ba21a0433baa7...` now has a reproducible classroom
match state: four fighters, TIME 120, Korean names and a player marker.
Two independent cold-boot input replays produced identical captured frames and
state bytes. The frame-5400 state is
`F:/GP32/results/resume17-gpfight/gpfight-match.state`, SHA-256
`14271b040adbc46eff8220e9c36b7876660e3698fef49a458c051c8088cd1e62`.
The frozen runner was 49ae3d5 (SHA-256
`af81fd0595c0d6d4ccd6c6f97a45cdeb8eb0d8369d700200fb71828c4fd2a09a`).

`match-input.txt` holds RIGHT from frame 60 to 300, then releases it. Direction
input changes the rendered simulation compared with an idle replay; START
shows Pause. Attack/guard inputs remain unresolved: A/B/L/R holds in this early
window matched idle. Do not label this a completed compatibility pass or proven
attack response. Round completion, later scenes and physical sound remain open.
Full commands and screenshots are in `resume17-gpfight/README.md` and `shots/`.
ROMs and saved states remain outside Git.

## GP Fight button cadence follow-up (resume19)

The bounded follow-up retained both idle and button-hold/tap replay frames.
Holding A from frame 60 still matches idle through the early 2000-frame window;
B/L/R holds show the same limitation there. A different script presses A for
four frames, releases it for sixteen, and repeats from frame 60 through 1700
(final release at 1704). Its captures
differ from idle from frame 200 onward, including fighters' positions and the
later combat HUD. This establishes a response to repeated A press/release in
the match; pixel changes alone do not prove the precise attack/guard mapping.

Parent replayed that script and the idle control to frame 1200 with runner
SHA-256 `66f09849645e86288649fc1735271c4c59b563f556eef10eb8eee427b950c709`.
Both frame-1000 and frame-1200 dumps exactly match the corresponding earlier
worker captures. The repeated-A script is
`F:/GP32/results/resume18-gpfight/combat-atap.txt`, SHA-256
`34cd30229daa3ba2f71bc81f86db59b65e23ef4848c421a0fe0284d6e39db61c`;
it uses the resume17 match-entry state above. Exact commands and results are in
`F:/GP32/results/resume19-gpfight/replay.json`, with existing-frame comparisons
in `resume18-gpfight/parent-comparison.json`.

The same 1200-frame repeated-A workload also matched between the Windows x86-64
and native H700 benchmark in all seven CPU/video/PCM fields. Both ended at
cycles 3729235000, PC `0x0c035efc`, CPSR `0x60000053`, with 216684 audio frames,
video hash `e78f9b6c4c98722c` and audio hash `235a169c190050c9`. This was one
unprimed exactness run per host without measured-clock qualification; its timing
must not be compared with the earlier 2400-frame movement-only performance
window. Evidence: `F:/GP32/results/resume19-gpfight/pc-h700.json`.

The idle timeline subsequently reaches GAME OVER and the title screen around
frames 3000-3600. A no-input loss is not a completed playable-game acceptance.
The late pale image from the earlier 2400-frame device test must not, on its
own, be labelled a renderer defect or a successful round transition. No input
mapping or timing change was made based on these inconclusive observations.

## Korean-language evidence (resume18)

Device inventory contains 20 `.smc` files, all named `(Korea)`, as checked via
SSH after resume17 deployment. Region labels establish which release is
installed; they do not alone prove that every screen is Korean. Existing
captures provide these narrower visual confirmations:

| Installed title | Korean text actually observed | Existing local capture |
| --- | --- | --- |
| Her Knights - All for Princess - Deadline | Title `그녀의 기사단 강행돌파`; Korean mode labels including `열혈모드`, `근성모드` | `F:/GP32/results/resume9-her3000.png`, `resume9-her4200.png` |
| Little Girl Mill of a Gingko, The | Title `은행나무소녀 밀`, `시작하기` / `끝내기`; Korean village dialogue | `F:/GP32/results/resume13-mil-r2-f7200.png`, `resume16-mill/can-d1300.png` |
| GP Fight | Korean fighter names including `나이수` and `주미라`; title `지피대난투` | `F:/GP32/results/resume17-gpfight/shots/08-match-state-f5400.png`, `resume18-gpfight/shots/idle-timeline-b.png` |

Little Wizard's inspected Korean-release combat capture
(`F:/GP32/results/resume13-wizard-korea-combat.png`) uses English fighter names
and numbers, so that frame cannot prove or disprove Korean text elsewhere.
The remaining installed titles (Astonishia Story R, Dooly Soccer 2002, Dungeon
& Guarder, Dyhard, Hany Party Game, Kimchiman, OneShot Voca, Princess Maker 2,
Rally Pop, Raphael, Tanggle's Magic Square, Therapy, Tomak, W.B.W., Wizard
Slayer, Woody & Kunta) still need game-specific language evidence. No ROM was
replaced or relabelled based on these incomplete visual observations, and no
whole-game translation or completion claim is made.

## Open items

- Korean Her Knights and Little Wizard now have H700 combat measurements;
  see the resume14-16 entries in [portability progress](PORTABILITY_PROGRESS.md)
  and [performance strategy](GP32_PERFORMANCE_STRATEGY.md). The older character
  selection and European combat-entry figures above are historical, distinct
  scenes. Do not combine states and ROMs from different releases.
- Homebrew and app enumeration is outside these asset roots (catalog doc section 4).
- Six catalogued Korean releases remain absent from supplied local assets, listed
  in [Spruce packaging](../packaging/spruce/README.md). Add owned dumps when
  supplied; catalog entries alone do not establish availability or compatibility.
- Later areas, bosses, complete playthroughs and physical audio/input latency
  remain unverified.


## Library coverage recheck (resume20)

A fresh local inventory reuses `scripts/korean_library.py` and its shared
payload/archive scanner. The top-level owned set contains 28 game variants,
grouped into 21 titles using the existing local MAME catalog snapshot's clone
relationships. Twenty titles have a Korea-labelled dump. This snapshot lists
26 Korea titles, leaving the same six unavailable locally: Funny Soccer 2002,
Holeman Battle Race 2002, Story of Bug eyed Monster, Tales of Windy Land,
Tears - Another Story and Winter Is. This is coverage of the recorded catalog,
not a claim that it exhausts every GP32 release or homebrew.

A fresh SFTP directory inventory confirms 20 ROM containers in the device's
`/mnt/SDCARD/Roms/GP32`, all labelled `(Korea)`. Nothing was added, downloaded,
renamed or removed from that library in this pass. Region labels still do not
prove Korean text throughout each game; the visual language evidence above
remains the narrower verified set.

The title grouping/report prototype stays outside the repository because it
depends on an untracked research snapshot and overlaps the existing scanner.
No extra maintenance tool or asset data is shipped. Evidence:
`F:/GP32/results/resume20-library/REPORT.md`, `titles-runA.json`, and
`F:/GP32/results/resume20-installed-library.json`.

## Korean library visual evidence (resume28, 2026-10-03)

The live device still contains the 20 Korea-labelled owned games. Current
Windows core f46ddac produced a 2,400-frame autopulse capture and an isolated
state for every title (20 process exits=0). This replaces neither gameplay
acceptance nor language verification: a loading/black/intro frame remains
unresolved. Six selected games were advanced with explicit input; short taps
were replaced with bounded held presses where menu navigation missed them.
No ROM download or user-save edit occurred.

| # | Title | Observed screen/progress | Hangul visible in captured evidence |
|---|---|---|---|
| 1 | Astonishia Story R | Title, then room/bed scene | Not established |
| 2 | Dooly Soccer 2002 | Loading screen only | Not established |
| 3 | Dungeon & Guarder - Dragon Gore | Difficulty menu, then stage background; active play unresolved | Not established |
| 4 | Dyhard - With Infinite Stairs | Korean new/load/save/return menu | Yes |
| 5 | GP Fight | Black at this capture; not a failure verdict | Not established |
| 6 | Hany Party Game | English start/options menu | Not established |
| 7 | Her Knights - All for Princess - Deadline | Korean game title | Yes |
| 8 | Kimchiman GP32 | Combat/HUD changes after movement and attack input | Not established |
| 9 | Little Girl Mill of a Gingko, The | Loading screen only | Not established |
| 10 | Little Wizard | Character selection | Not established |
| 11 | OneShot Voca | Korean learning/search UI | Yes |
| 12 | Princess Maker 2 | Korean name entry | Yes |
| 13 | Rally Pop | Level selection | Not established |
| 14 | Raphael | Intro background only | Not established |
| 15 | Tanggle's Magic Square | Puzzle field with Korean mode text | Yes |
| 16 | Therapy | Korean subtitle on intro/title | Yes |
| 17 | Tomak - Save the Earth, Again | Shooting stage with enemies and HUD | Not established |
| 18 | W.B.W. - Wanna Be Wizard_ | English publisher intro | Not established |
| 19 | Wizard Slayer | Korean stage name and health/magic HUD, then action scene | Yes |
| 20 | Woody & Kunta - Treasure Island | Korean character dialogue | Yes |

Screenshots and exact replay commands: `F:/GP32/results/resume28-korea/`
(`inventory.json`, `contact.png`, numbered folders, `advance-contact.png`).
Eight titles have direct Hangul evidence in this pass; it does not prove all
of their content is localized. The other labels alone are not language proof.

New PC/H700 replays begin from observed Kimchiman, Tomak and Wizard Slayer
action scenes. With the same state/input and 300 warmup + 600 measured frames,
all seven CPU/video/audio fields match on all three. H700 raw throughput was
90.787/71.320/120.741 fps respectively, but clocks ramped during the runs: these
are neither a controlled ranking nor proof of stable real-frontend 60fps.
Audio delivery/physical input latency and later-game compatibility remain open.
Evidence: `F:/GP32/results/resume28-korea/combat-equivalence.json`. Kimchiman
uses the f46ddac Windows core; Tomak/Wizard Slayer use the callback-exit candidate.
The H700 executable is f46ddac for all three.

The six missing Korean releases listed above still need owned game files.

## Loading-screen followup (resume29, 2026-10-03)

Two unresolved resume28 captures now have later visual evidence:

* **Dooly Soccer 2002:** resuming the loading state without BIOS autopulse
  reaches the title within another 600 frames, then Korean story dialogue.
  Explicit held input clears the dialogue and reaches a match: screenshots
  progress from 0:00 / 0:0 to 0:21 / 0:1, with players and ball on the pitch.
  This rules out a permanent loading stall for this replay. It does not
  establish a complete match, audio quality or save compatibility.
* **W.B.W. - Wanna Be Wizard_:** another 1,800 frames from the publisher
  capture, with 15-frame A presses every 120 frames and BIOS autopulse off,
  reaches a forest scene with a character, wolves, date/currency HUD and
  Hangul location text. Later-stage play and audio remain unverified.

Evidence: `F:/GP32/results/resume29-dooly/FINDINGS.md`, `runs.txt`,
`play600.png`, `play2400.png`, and
`F:/GP32/results/resume29-wbw/advance.json`, `advance.png`.
These add direct Hangul evidence for two more of the 20 existing owned titles.
The library and user saves were not modified; missing owned Korean files are
still required for the six catalog gaps above.

## Tomak real frontend audio (resume31, 2026-10-03)

The observed Tomak shooting scene now has a bounded real H700 RetroArch
replay: 900 emulated frames from `resume28-korea/17/advance.state`, using the
same movement/attack script as the PC/H700 comparison. The unmodified
resume30 core produces 347,032 source stereo frames and nonzero samples in
every replay frame. Guest PC, source frame count, nonzero count and IIS state
match the PC diagnostic frame by frame; the CPU endpoint matches the prior
benchmark. RetroArch accepts all 661,990 submitted resampled stereo frames
(including its initial BIOS run), without partial/zero callbacks, pending
audio, ALSA errors or recovery. The screenshot still shows the player,
enemies and HUD in combat.

In the measured last 600 frames, core work averages 8.043 ms and reaches
11.501 ms maximum; presentation interval p99 is 17.205 ms. All recorded CPU
clock samples are 1,512 MHz. This is a 15-second replay, not complete-game
acceptance, a physical listening test, or proof of universal 60fps.
Evidence: `F:/GP32/results/resume31-audio/probe.csv`,
`resume31-audio-runtime-verified.json`, `resume31-audio-summary.json`,
`resume31-audio-runtime.png`. User settings, ROMs and saves are unchanged.
