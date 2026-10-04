# GP32 catalog and heavy-title targets (resume12 research)

## Counting correction (resume72, 2026-10-04)

The original supplied library has 28 game ZIPs, but only 21 distinct titles:
20 Korea-labelled titles and Blue Angelo, plus seven regional/revision
duplicates. This is not the same as the commonly cited 28-commercial-title
list. The previous phrase "six missing Korean releases" incorrectly implied
confirmed retail status for every missing MAME entry.

Funny Soccer, Story of Bug Eyed Monster and Winter Is appear in the commercial
list. Holeman Battle Race and Tales of Windy Land have disputed/unreleased
status. Tears - Another Story has unresolved demo/final status in MAME's own
notes and its [developer discussion](https://forums.bannister.org/ubbthreads.php?Number=56755&page=all&ubb=showflat).
These six entries are not six ordinary homebrew additions. MAME dump coverage,
retail release counts and regional ZIP counts must be reported separately.

All six missing SMC payloads have now been downloaded and match the SHA-1s in
the official [MAME GP32 list](https://github.com/mamedev/mame/blob/master/hash/gp32.xml).
The download manifest is `F:/GP32/downloads/verified/missing-six-manifest.json`.
Hash identity establishes the catalogued dump, not full-game compatibility or
whether an unreleased/demo-labelled product became a retail release. Blue
Angelo has also been restored to the device library and remains a priority
heavy-game target even though the supplied edition is European.

The research snapshot below remains historical; its earlier missing-file
statements do not describe the newly downloaded library.

Research pass 2026-10-03. Scope: MAME's official GP32 software list
(hash/gp32.xml, master) with region variants counted separately, cross-checked
against the commercial release list and the undumped-title notes in historical
sources. This document does not rank titles by speed; section 5 separates what is
measured from what is still a hypothesis.

Raw source copies live outside the repository, under F:/GP32/results/:

- resume12-research-mame-gp32.xml - MAME hash/gp32.xml, master, fetched 2026-10-03,
  SHA-256 D4A28D1DB8A2197AD5F6B247E03AE759AD9ADC98352357C245D57926E0783885
- resume12-research-mame-gp32.cpp - MAME src/mame/gamepark/gp32.cpp, master,
  fetched 2026-10-03, SHA-256
  F253AB55BD37B0E0CCB480BFDD8D51394B23C796D79F3702D67C8426AC7A9866
- resume12-research-ko-wiki-GP32.wiki - Korean Wikipedia GP32 article (the page
  MAME's XML header cites for its undumped list), SHA-256
  4A3E02FAD96656BE11C5C992B936FD5FDC8A868221B0EE344ED31A76F538704E
- resume12-research-en-wiki-GP32.wiki - English Wikipedia GP32 article, SHA-256
  3817B62350233B43D4FF3B5FDAEB7257A2982A2EC35A4ACD816DE914D5EB7EB7
- resume12-research-catalog.json - machine-readable parse of all 38 MAME entries
  (set, cloneof, year, publisher, card image size, CRC, SHA-1), SHA-256
  18CE3FA5DF28BD428B94ECDC4CD627B84334FB6A231B763BAE7C9010534B32B0

Secondary sources used only for cross-checking, not fetched locally: English
Wikipedia "List of commercial GP32 games" (28 titles, 2023-11 mirror snapshot)
and a game.donga.com 2003-04-04 article for Winter Is (developer Rosa:6,
published by Gamepark).

## 1. Counts from MAME hash/gp32.xml

| Metric | Count |
| --- | --- |
| software entries | 38 |
| unique titles (no cloneof) | 31 |
| region or revision clones | 7 |
| Korea-only titles | 20 |
| Europe-only titles | 5 |
| titles with both Korea and Europe variants | 6 |
| entries on a 17,302,528-byte card image | 34 |
| entries on a 34,604,032-byte card image | 2 (Astonishia Story R, Funny Soccer 2002) |
| entries on a 69,207,040-byte card image | 2 (Story of Bug eyed Monster, Winter Is) |
| supported status | partial, all 38 |

The XML header states that all games "have a cap of partial support because not
extensively tested given the very demanding host CPU usage", that reload after
save needs counterchecking for every game, and that firmware 1.6.6 is the
recommended default BIOS. The card image size is the size of the dumped
SmartMedia image (card capacity plus card overhead), not a measured payload size.

## 2. Full MAME catalog, clones separated (38 entries)

| # | MAME set | cloneof | Description (as MAME) | Year | Publisher (as MAME) | Card image bytes | SHA-1 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | astonish | - | Astonishia Story R (Korea) [어스토니시아 스토리 R] | 2002 | Gamepark | 34604032 | c894b0dd8b934c77580187d7972490f5e59f2372 |
| 2 | blueangl | - | Blue Angelo - Angels from the Shrine (Europe) | 2005 | Shibuya Interactive | 17302528 | 369f96bb41378ab4108d0dc8866148b328cbd75e |
| 3 | doolysoc | - | Dooly Soccer 2002 (Korea) [둘리축구] | 2002 | Gamepark | 17302528 | 0347cd106cba22b0d2e1170d44ec1d302e982a5e |
| 4 | dunguard | - | Dungeon & Guarder (Korea) [던전 앤 가더] | 2001 | Gamepark | 17302528 | 93c358177ae221f33e0aef793f83cc027ede0cfc |
| 5 | dunguarde | dunguard | Dungeon & Guarder (Europe) | 2001 | Gamepark | 17302528 | 433da4a0cdbf3c797f62bf01bfa86608434b04f2 |
| 6 | dyhard | - | Dyhard - Infinity (Korea, V1083) [다이하드] | 2001 | Gamepark | 17302528 | 091c0f41954c03b9979683dd957a9dbbc4e28405 |
| 7 | dyharde | dyhard | Dyhard - Infinity (Europe, V1107) | 2001 | Gamepark | 17302528 | b684b443b1450f98a43953393352b0724a9171cb |
| 8 | gpdaenan | - | GP Daenantu ~ GP!!! Dairantou (Korea) [지피대난투] | 2003 | Gamepark | 17302528 | 3bb0dfba690eb8b3885e48f4c0a981b8c96cda9b |
| 9 | hanypart | - | Hany Party Game (Korea) | 2002 | Gamepark | 17302528 | d7cf21ba51c7fea4257a7309eb31f96e635c783b |
| 10 | herknite | - | Geunyeoui Gisadan Ganghaengdolpa (Korea) [그녀의 기사단 강행돌파] | 2002 | Gamepark | 17302528 | 4ced58d61f831cbfc0d0a369e559ad364cb71541 |
| 11 | herknitee | herknite | Her Knights - All for the Princess (Europe) | 2002 | Gamepark | 17302528 | d137f8ed7e2a092037d64d8cf65bace68e2d6ebb |
| 12 | kimchimn | - | Kimchiman GP32 (Korea) [김치맨] | 2002 | Gamepark | 17302528 | 593ff3500abb654e297f2f66cae92ec893db6906 |
| 13 | mil | - | Eunhaengnamu Sonyeo Mil (Korea) [은행나무소녀 밀] | 2003 | Gamepark | 17302528 | 1ce508583d7bdd176209c2ef3a606e7686590b87 |
| 14 | ltwizard | - | Little Wizard (Korea) [리틀위저드] | 2001 | Gamepark | 17302528 | 27387e388dabb3bf78ab2d4cac1b8a0104f62e49 |
| 15 | ltwizarde | ltwizard | Little Wizard (Europe) | 2001 | Gamepark | 17302528 | ef1cae9d810ee55aeefb86410843241503df30f1 |
| 16 | oneshot | - | One-Shot VOCA (Korea) [원샷 VOCA] | 2002 | Gamepark | 17302528 | 69f1f554077397cccc17cc66c3e0f520005fa439 |
| 17 | prinmak2 | - | Princess Maker 2 (Korea) [프린세스메이커2] | 2002 | Gamepark | 17302528 | 2a55ffc4b69ac6515903b11d5c1a67559ed42891 |
| 18 | rallypop | - | Rally Pop (Korea) [랠리팝] | 2001 | Gamepark | 17302528 | f7e8c59c88adcce506cb6b8a86fd2ad9b307468f |
| 19 | raphael | - | Raphael (Korea) | 2002 | Gamepark | 17302528 | 9d4e4dd297dd33ecfa22a90826c0efccb47be323 |
| 20 | tanggle | - | Tanggle's Magic Square (Korea) | 2001 | Gamepark | 17302528 | 3dccead2a6e1fa11ec5d66f248eeacf9f25106d5 |
| 21 | therapy | - | Therapy (Korea) [테라피] | 2002 | Gamepark | 17302528 | 7ded192d9dce09c92dc5a83ccb0b477219f33b11 |
| 22 | tomak | - | Tomak - Save the Earth, Again (Korea, v2.0?) [토막 지구를 지켜라 Again] | 2002 | Gamepark | 17302528 | 87b196f94bb5361563d7b35ba732b56e214be08e |
| 23 | tomake | tomak | Tomak - Save the Earth, Again (Europe, v1.0) | 2002 | Gamepark | 17302528 | 0767d37cd7dd29bb572a82cc05d766e8e7321dd9 |
| 24 | tomake2 | tomak | Tomak - Save the Earth, Again (Europe, v2.0) | 2002 | Gamepark | 17302528 | 9c0e29cfa7be8a6644c63499b7065d4e5f40208e |
| 25 | treasisl | - | Treasure Island (Korea) [보물섬] | 2002 | Gamepark | 17302528 | 9e768faa8300cd5b3102f433aae53a6f4d3d7f1f |
| 26 | treasisle | treasisl | Treasure Island (Europe) | 2002 | Gamepark | 17302528 | cd1df6228293dc8193e779bdba562f3040e21b4a |
| 27 | wbw | - | W.B.W. - Mabeobsaga Doego Sip-eo! (Korea) [마법사가 되고 싶어!] | 2004 | Gamepark | 17302528 | cb19613a9377aef5be77bb00138fd8d0b201737e |
| 28 | wizardsl | - | Wizard Slayer (Korea) | 2002 | Gamepark | 17302528 | a6801cee5cdc2f76d318df1880d8441bf9405de3 |
| 29 | funnysoc | - | Funny Soccer 2002 (Korea) | 2002 | Gamepark | 34604032 | 70b12322b3e5245132e167b0d1633556a048b257 |
| 30 | gloopdx | - | GlooP Deluxe (Europe) | 2003 | Aeon Flame | 17302528 | a2440b563e6ad36f9718a5a9db17ed6d219e0e9e |
| 31 | holbatra | - | Holeman Battle Race 2002 (Korea) | 2002 | MoongKle | 17302528 | 8e7c3a36d230afbdf465ba62fb3785f1376c9373 |
| 32 | pinbdrea | - | Pinball Dreams (Europe) [핀볼즈드림] | 2002 | Gamepark | 17302528 | 19eefbfd765913067a5a622ae6c40222118e6e5d |
| 33 | sobemons | - | Story of Bug eyed Monster (Korea) | 2003 | Gamepark | 69207040 | 3e70e746f9a1819e629ffc4b9082ac4a8d1d022e |
| 34 | winteris | - | Winter Is (Korea) [겨울은...] | 2003 | Gamepark | 69207040 | cf072fae0825164ff8c7393ab513058b78c46061 |
| 35 | suplusha | - | Super Plusha (Europe) [슈퍼 플루샤] | 2002 | Gamepark | 17302528 | 1e2e984014f184759a35df93160f0fb4d11c7742 |
| 36 | talowila | - | Tales of Windy Land (Korea) | 2003 | AIM Technology | 17302528 | 51679a8197f721fc601ea0cd8c91a3bbabed9585 |
| 37 | totogogo | - | Topy Topy Gogo (Europe) | 2003 | Gamepark | 17302528 | 46e1f08ec260a633e8e4c44b3b46b5135e6c76a6 |
| 38 | tearsast | - | Tears - Another Story (Korea) | 2003 | Team D.T.R. | 17302528 | 682f123d3f827544cc876e624ccacd61cd8283a3 |

Korean titles in brackets are taken from the Korean Wikipedia article fetched in
this pass; a dash in cloneof means the set is not a clone.

## 3. Clone groups

| Family | Sets |
| --- | --- |
| Dungeon & Guarder | dunguard (Korea, 2001), dunguarde (Europe) |
| Dyhard | dyhard (Korea, V1083), dyharde (Europe, V1107) |
| Her Knights | herknite (Korea, "Deadline"), herknitee (Europe, "All for the Princess") |
| Little Wizard | ltwizard (Korea, 2001), ltwizarde (Europe) |
| Tomak | tomak (Korea, v2.0?), tomake (Europe v1.0), tomake2 (Europe v2.0) |
| Woody & Kunta Treasure Island | treasisl (Korea), treasisle (Europe) |

## 4. Catalog vs release list and unresolved identities

- English Wikipedia states about 28 commercial games were released, five at the
  launch on 2001-11-23, and that the last commercial game was Blue Angelo on
  2004-12-16 (MAME's dump year for it is 2005).
- The 28-title commercial list and MAME's 31 unique titles differ by exactly five
  names. List-only: Tears: Contact, which MAME's XML header itself flags with
  "not sure this game title exist for the GP32". MAME-only: Tears - Another Story
  (Team D.T.R.), Holeman Battle Race 2002 (MoongKle), Tales of Windy Land (AIM
  Technology), Topy Topy Gogo.
- MAME's XML header marks the 달려라 하니 (Ride Do / Run Hani) series as undumped
  and leaves 강행돌파 questionable, possibly the same game as herknite.
- Story of Bug eyed Monster identity conflict: MAME lists publisher Gamepark;
  Korean Wikipedia credits Team Athena; the commercial list credits Article
  Seezak with Gamepark publishing and a 2003-09-09 release. Both claims stay here
  as unresolved.
- Distribution: boxed SmartMedia or an encrypted per-device download; JoyGP was
  the international store, MegaGP the earlier Korea-only one. Blue Angelo was
  boxed only (French production); GlooP Deluxe was online only and not via JoyGP.
  Hany Party Game shipped as one product while its two games (Star Ball, Tower
  Master) were also sold separately through JoyGP.
- MAME's hash file cannot separate utility, freeware and retail software: its
  publisher field says Gamepark for most entries. One-Shot VOCA is the only entry
  whose title indicates a non-game utility. Classification of GlooP Deluxe,
  Super Plusha, Topy Topy Gogo, Tears - Another Story, Tales of Windy Land and
  Holeman Battle Race 2002 stays open until a per-title source is added.
- Homebrew and freeware are not enumerated by MAME's hash file. Per the English
  article, GamePark published user homebrew on its site and required an encrypted
  Free Launcher for unsigned software. A sourced homebrew/app enumeration is an
  open item; this pass does not guess it.

## 5. Priority heavy targets and their actual scenes

Measured numbers are core-only headless throughput from this repository's own
docs (docs/PORTABILITY_PROGRESS.md, docs/HER_KNIGHTS_KOREA_BENCHMARK.md) on H700
at a matched 1.512 GHz observation, all for the retained lazy cache epoch. They
are not RetroArch presentation fps, not game animation rates, and not game-wide
minima.

| Priority | Title (MAME set) | Actual scene | Evidence status |
| --- | --- | --- | --- |
| 1 | Her Knights: All for Princess - Deadline (herknite) | first palace battle against Lynnerd, 1200 warmup / 1200 measured frames, scripted move and attack input | measured 90.5365 core fps on H700 in matched-clock ABBA (81.986 before the lazy epoch, +10.43%); later stages with more enemies unmeasured |
| 2 | Little Wizard (ltwizard) | character selection, 2400/300 frames | measured 50.493 core fps in matched-clock ABBA (47.7405 before the lazy epoch, +5.77%); real combat entry state now exists (F:/GP32/results/resume11-wizard-combat-entry.state frame 3350 and resume11-wizard-combat-x3900.state) with inspected captures, but H700 throughput for actual combat is not yet measured |
| 3 | Blue Angelo - Angels from the Shrine (blueangl) | extracted gameplay state; 1200/300 window for the retained H700 number; boss and large-sprite scenes unmeasured | measured 66.806 core fps on H700 (54.7035 before the lazy epoch, +22.12%, all exactness fields matching); 532.537 core fps on a 9800X3D Win64 run with matching CPU, video and PCM hashes |
| 4 | Tomak - Save the Earth, Again (tomak / tomake / tomake2) | stage 1 opening plus a mid-stage effect and bullet wall, Korean v2.0 dump first | hypothesis: per-frame sprite workload of a 2002 shooter; no measurement or per-title demand source found in this pass |
| 5 | Funny Soccer 2002 (funnysoc) | one full 11-a-side match in progress | hypothesis: sports AI and sprite scene workload; shares the 34,604,032-byte card class with Astonishia Story R; no measurement found in this pass |

No ordering claim is made between titles. Her Knights, Little Wizard and Blue
Angelo are the existing bench, and the project docs state their scenes are
different fixed scenes, explicitly not a ranking of the hardest GP32 titles.
Tomak and Funny Soccer 2002 are the next capture candidates for the reasons in
the table, and must not be described as the most demanding titles until one
repeatable scene each has been measured. Other unmeasured catalog titles that
could carry heavy scenes and should be checked later: GP Fight (gpdaenan, 2003
versus fighting), Dungeon & Guarder, Rally Pop, Astonishia Story R. Four-player
GP Link play (Little Wizard, Dungeon & Guarder, Treasure Island, Rally Pop) is a
separate workload and is not modeled by the existing single-input harness.

The Little Wizard combat states exist because of a separate scene exploration
(F:/GP32/results/resume11-wizard-combat.txt reproduces entry from boot); the old
2700-frame Wizard benchmark was character selection, and repeated identical
intermediate captures were a headless dump-at scheduling defect, not an emulation
hang. Bulk boot/menu throughput for the whole local set is produced by
scripts/bench_catalog.py --execute and summarised in docs/GP32_LOCAL_GAME_MATRIX.md;
those 2400 warmup / 300 frame windows are boot/menu windows, not gameplay and not
a pass gate.

Capture spec for each target: one repeatable state saved from a read-only ROM,
run with the existing external harness pattern (--bios gp32166m.bin --smc ...
--state ... --input-script ... --warmup N --frames N --jit), ABBA ordering, a
matched clock, recorded frequency and temperature, and CPU, video and PCM hash
comparison.

## 6. Open items

1. Fetch the Wayback copy of the GamePark product page for an official release list.
2. Resolve Tears: Contact against Tears - Another Story, and check whether the
   달려라 하니 series was actually released on SmartMedia.
3. Collect per-game 133 MHz / clock-mode evidence from the archived gp32x
   (pyra-handheld) threads to support demand claims.
4. Obtain the Tomak and Funny Soccer 2002 manuals to name concrete stage scenes.
5. File-level local inventory is produced by scripts/bench_catalog.py into
   F:/GP32/results/resume12-catalog-inventory.* and summarised in
   docs/GP32_LOCAL_GAME_MATRIX.md; this catalog document does not duplicate it.
6. Measure the two existing gameplay states on H700 with the frozen runner, one at
   a time (--limit 1 --state): the Her Knights combat state and the new Little
   Wizard combat-entry state.
