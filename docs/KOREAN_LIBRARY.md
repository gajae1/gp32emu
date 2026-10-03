# Korean GP32 library inventory (scripts/korean_library.py)

Read-only inventory for one local folder of GP32 dumps. It reports which files
are Korea-tagged, the SHA-256 of every unique payload, which copies are
duplicates, what is BIOS, and what is untagged, malformed or ambiguous. It
never downloads, extracts, renames or deletes anything and never touches a
device.

## Installed Korean library (2026-10-04, resume53)

The current local inventory contains 20 unique Korea-tagged game payloads.
The H700 library contains exactly those 20 payloads, with no missing or extra
games; the fresh comparison uses SHA-256, not filenames alone. Raw and ZIP
copies of the same local game count once. BIOS files are excluded from the
game menu. Evidence: `F:/GP32/results/resume53-diag/korean-library.json`,
`library-device.json` and `installed-sha256.txt` in the same directory.

Displayed-language evidence is cumulative from the captures documented in
[the local game matrix](GP32_LOCAL_GAME_MATRIX.md), including its resume46
Dungeon & Guarder followup. It is separate from the inventory tool's
filename-based classification; the tool still reports language as unverified.

| Captured language evidence | Installed Korea-tagged titles |
| --- | --- |
| Hangul visible (13) | Dooly Soccer 2002; Dungeon & Guarder; Dyhard; GP Fight; Her Knights; Little Girl Mill; OneShot Voca; Princess Maker 2; Tanggle's Magic Square; Therapy; W.B.W.; Wizard Slayer; Woody & Kunta |
| Not yet established (7) | Astonishia Story R; Hany Party Game; Kimchiman GP32; Little Wizard; Rally Pop; Raphael; Tomak |

The six catalogued Korea-region releases without supplied dumps are Funny
Soccer 2002, Holeman Battle Race 2002, Story of Bug eyed Monster, Winter Is,
Tales of Windy Land and Tears - Another Story. Adding those requires the
missing dumps. No game was downloaded or removed during this verification.
Neither a Korean-region label nor one Hangul capture establishes complete
localization, whole-game compatibility or a complete GP32 collection.

ZIP reading and hashing come from `scripts/bench_catalog.py` (`sha256_file`,
`inspect_zip`), so both tools share one implementation and one set of archive
limits; keep the two files together in `scripts/`. Archive status names below
are bench_catalog's.

## Usage

```
python scripts/korean_library.py <folder> [--recursive] [--json | --json-out PATH]
```

The folder is scanned at its top level; `--recursive` adds subfolders (symlinks
and junctions are skipped). Progress lines go to stderr, the summary or the
`--json` document goes to stdout.

`--json-out` must end in `.json` and is refused with exit 2 if it resolves to
any scanned source file (realpath and hardlink comparison), so a report can
never overwrite a ROM.

```
python scripts/korean_library.py F:/GP32
python scripts/korean_library.py F:/GP32 --recursive   # also picks up build/ and results/ fixtures
python scripts/korean_library.py F:/GP32 --json-out inventory.json
python scripts/korean_library.py F:/GP32/test-assets   # support folder: Europe copies + BIOS binary
```

Exit codes: 0 = no problems, 1 = problems flagged (see `problems`), 2 = usage
error or missing folder.

## Output

stdout: counts, the Korea unique payload list (SHA-256 prefix, size, copy
count), BIOS-labelled files and problems. `--json` prints the deterministic
document instead; keys and rows are sorted and no timestamp is embedded, so a
re-run over the same folder is byte-identical (verified below).

Top-level keys: `tool`, `schema`, `scan`, `language_policy`, `counts`,
`problems`, `unique`, `files`.

- `counts`: `files`, `by_class`, `by_status`, `unique_payloads`,
  `unique_by_class`, `duplicate_copies`, `label_conflicts`, `problems`.
- `unique`: one record per payload SHA-256 with `classification`, `labels`,
  `primary_path`, `primary_kind`, `primary_entry`, `copies`, `members`.
- `files`: one row per scanned file with path/size, status, note, flags, the
  selected payload, the zip member list (capped at 64 entries), classification
  basis and dedup assignment.

## Classification

| Class | Rule |
| --- | --- |
| `korea` | explicit Korea tag in the file name or in a ROM member name |
| `other` | explicit non-Korea region tag (Europe, Japan, USA, Asia, World, China, Taiwan) |
| `bios` | explicit `[BIOS]` / `(BIOS)` marker in the file or member name |
| `ambiguous` | conflicting region labels, for example `(Korea).zip` holding `(Europe).smc` |
| `unknown` | no region or BIOS tag at all |

Region is not language. A `(Korea)` tag is a dump label, not proof that the
in-game text is Korean: every record carries `language_verified: false`, the
JSON `language_policy` string says so, and ROM contents are hashed but never
interpreted to verify language.

## Dedup, statuses and flags

Dedup key is the payload SHA-256: a raw file hashes itself, a `.zip` streams
its single `.smc`/`.fxe`/`.fpk` member in memory with CRC checking and no
extraction. A group's primary copy prefers the raw ROM over the archive; copies
that carry different region labels are reported as `label-conflict` problems.

Statuses (from `bench_catalog.inspect_zip`, plus raw-file equivalents): `ok`,
`no-rom-entry`, `ambiguous-zip` (more than one ROM member; nothing is
auto-selected), `zip-read-error` (malformed, truncated or CRC failure),
`entry-too-large`, `zip-ratio-suspect`, `zip-read-timeout`, `unreadable`,
`hash-timeout`. The `unsafe-member-name` flag marks archives whose ROM member
uses an absolute or traversal name. `problems` collects everything that is not
`ok` or `no-rom-entry`, plus region conflicts; a BIOS archive with no ROM
member is expected and is not a problem.

## Validation (2026-10-03, this workspace)

```
python scripts/korean_library.py F:/GP32 --json-out <temp>/kr-a.json
python scripts/korean_library.py F:/GP32/test-assets --json-out <temp>/kr-c.json
```

- F:/GP32: 38 files (22 korea, 8 other, 8 bios), 29 unique payloads (20 korea,
  8 other, 1 BIOS binary), 2 duplicate copies collapsed (Astonishia Story R and
  Her Knights - Deadline, raw `.smc` plus `.zip`), 0 problems, exit 0.
- F:/GP32/test-assets: 5 files; 4 Europe payloads and the v1.6.6 BIOS binary.
- Across both scans: 36 payload rows collapse to 30 unique payloads - the 28
  game payloads match docs/GP32_LOCAL_GAME_MATRIX.md - with 6 duplicate copies.
- The 20 Korea payload SHA-256 values equal exactly the device-verified
  installed set in F:/GP32/results/resume12-korean-library.json.
- A repeat run of the first command produced byte-identical JSON (50,998 bytes).

## Limitations

1. Region tags are dump labels, not verified language: payloads are hashed but
   not interpreted, so no claim is made about in-game text.
2. No release catalog is built in: the tool reports what the folder holds and
   never what is missing. Known gaps come from docs/GP32_GAME_CATALOG.md - as
   of 2026-10-03 six Korea-region MAME entries had no local payload: Funny
   Soccer 2002 (funnysoc), Holeman Battle Race 2002 (holbatra), Story of Bug
   eyed Monster (sobemons), Winter Is (winteris), Tales of Windy Land
   (talowila), Tears - Another Story (tearsast). That list is catalog-derived,
   not language-verified, and not produced by this tool.
3. BIOS detection needs an explicit marker. A renamed dump such as the device's
   `gp32166m.bin` has neither a marker nor a ROM extension and is not scanned.
   Scanned extensions: `.smc`, `.fxe`, `.fpk`, `.zip`, plus any file whose name
   carries a BIOS marker; homebrew outside those is out of scope.
4. Archives whose payload is a non-ROM member or lives in several members stay
   `no-rom-entry` / `ambiguous-zip` and are not hashed; nothing is extracted to
   work around that.
5. No network access and no MAME verification; comparing payload SHA-1 values
   against docs/GP32_GAME_CATALOG.md stays a manual step.
