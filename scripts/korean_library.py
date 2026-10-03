#!/usr/bin/env python3
"""GP32 Korean-library inventory (read-only, local, no network).

Scans one folder for GP32 containers (.smc/.fxe/.fpk/.zip) plus files whose
name carries an explicit [BIOS] marker, hashes ROM payloads with the helpers
shared with scripts/bench_catalog.py (sha256_file, inspect_zip), collapses
identical payloads across raw files and archives, and labels each file korea /
other / bios / ambiguous / unknown from explicit name tags only.

Region is not language: "(Korea)" is a dump label, not proof of the in-game
text, so every record carries language_verified=false. Archives are opened
read-only and never extracted, renamed or deleted. JSON is deterministic
(sorted keys and rows, no timestamp); the summary or JSON goes to stdout and
the per-file progress lines go to stderr.

Usage:
  python scripts/korean_library.py F:/GP32
  python scripts/korean_library.py F:/GP32 --recursive
  python scripts/korean_library.py F:/GP32 --json-out inventory.json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

try:
    from bench_catalog import CONTAINER_EXTS, ROM_EXTS, inspect_zip, sha256_file
except ImportError:
    sys.stderr.write("error: keep korean_library.py next to bench_catalog.py in scripts/\n")
    raise SystemExit(2)

TOOL = "korean_library.py"
SCHEMA = 1
MAX_MEMBERS = 64
MAX_ENTRY_BYTES = 128 * 1024 * 1024
MAX_ZIP_RATIO = 200.0
STREAM_BUDGET = 120
CLEAN_STATUSES = ("ok", "no-rom-entry")
KIND_RANK = {"rom": 0, "file": 1, "zip": 2}
REGIONS = {"korea": "Korea", "europe": "Europe", "japan": "Japan", "usa": "USA",
           "asia": "Asia", "world": "World", "china": "China", "taiwan": "Taiwan"}
TAG_RE = re.compile(r"[\(\[]([^\)\]]*)[\)\]]")
BIOS_RE = re.compile(r"\[bios\]|\(bios\)", re.I)
POLICY = ("region comes from explicit name tags ('(Korea)', '[BIOS]') only; the name is a "
          "dump label, not proof of the in-game language, so language_verified is false "
          "for every record and untagged files stay 'unknown'")


def ext(name):
    return os.path.splitext(name)[1].lower()


def region_tags(name):
    tags = []
    for group in TAG_RE.findall(name):
        for token in re.split(r"[,/+;]", group):
            canon = REGIONS.get(token.strip().lower())
            if canon and canon not in tags:
                tags.append(canon)
    return tags


def is_bios(name):
    return bool(BIOS_RE.search(name))


def is_link(path):
    if os.path.islink(path):
        return True
    is_junction = getattr(os.path, "isjunction", None)
    return bool(is_junction and is_junction(path))


def collect_files(folder, recursive):
    files = []

    def keep(path):
        name = os.path.basename(path)
        if name.startswith(".") or is_link(path) or not os.path.isfile(path):
            return
        if ext(name) in CONTAINER_EXTS or is_bios(name):
            files.append(os.path.abspath(path))

    if recursive:
        for base, dirs, names in os.walk(folder, followlinks=False):
            dirs[:] = sorted(d for d in dirs if not d.startswith(".")
                             and not is_link(os.path.join(base, d)))
            for name in sorted(names):
                keep(os.path.join(base, name))
    else:
        for name in sorted(os.listdir(folder)):
            keep(os.path.join(folder, name))
    return files


def classify(name_tags, member_tags):
    tags = name_tags + [t for t in member_tags if t not in name_tags]
    non_korea = [t for t in tags if t != "Korea"]
    reason = None
    if "Korea" in tags and non_korea:
        reason = "Korea and %s" % ", ".join(non_korea)
    elif name_tags and member_tags and set(name_tags) != set(member_tags):
        reason = "container %s vs member %s" % (", ".join(name_tags), ", ".join(member_tags))
    if reason:
        return "ambiguous", "region labels conflict: %s" % reason, reason
    if not tags:
        return "unknown", "no explicit region or BIOS tag", None
    where = " and ".join(w for w, t in (("file name", name_tags), ("archive member", member_tags)) if t)
    if "Korea" in tags:
        return "korea", "explicit (Korea) tag in %s" % where, None
    return "other", "explicit %s tag in %s" % (", ".join(tags), where), None


def scan_file(path):
    name = os.path.basename(path)
    kind = "zip" if ext(name) == ".zip" else ("rom" if ext(name) in ROM_EXTS else "file")
    row = {"path": path, "name": name, "kind": kind, "size": os.path.getsize(path),
           "status": "ok", "note": None, "flags": [], "payload_sha256": None,
           "payload_size": None, "entry": None, "entries": [], "entries_truncated": False,
           "classification": None, "basis": "", "ambiguity": None,
           "language_verified": False, "dedup_group": None, "primary_path": None,
           "is_duplicate": False}
    member_tags, member_bios = [], False
    if kind == "zip":
        try:
            info = inspect_zip(path, MAX_ENTRY_BYTES, MAX_ZIP_RATIO, STREAM_BUDGET)
        except (OSError, TimeoutError) as exc:
            row["status"] = "unreadable" if isinstance(exc, OSError) else "hash-timeout"
            row["note"] = str(exc)
        else:
            row["status"], row["note"] = info["status"], info["note"]
            row["entry"], row["payload_sha256"], row["payload_size"] = (
                info["entry"], info["payload_sha256"], info["payload_size"])
            entries = info["entries"]
            row["entries"] = entries[:MAX_MEMBERS]
            row["entries_truncated"] = len(entries) > MAX_MEMBERS
            member_bios = any(is_bios(item["name"]) for item in entries)
            member_tags = region_tags(info["entry"] or "")
            for item in entries:
                if item["rom"]:
                    member_tags += [t for t in region_tags(item["name"]) if t not in member_tags]
            if any(item["unsafe"] and item["rom"] for item in entries):
                row["flags"].append("unsafe-member-name")
    else:
        try:
            row["payload_sha256"], row["payload_size"] = sha256_file(path, STREAM_BUDGET)
        except (OSError, TimeoutError) as exc:
            row["status"] = "unreadable" if isinstance(exc, OSError) else "hash-timeout"
            row["note"] = str(exc)
    file_bios = is_bios(name)
    if file_bios or member_bios:
        where = "file and member names" if file_bios and member_bios else (
            "file name" if file_bios else "archive member name")
        row["classification"] = "bios"
        row["basis"] = "explicit BIOS marker in %s" % where
    else:
        row["classification"], row["basis"], row["ambiguity"] = classify(
            region_tags(name), member_tags)
    return row


def build_inventory(folder, recursive, files):
    rows = []
    for path in files:
        row = scan_file(path)
        sys.stderr.write("scan %-16s %-9s %s\n" % (row["status"], row["classification"], row["name"]))
        rows.append(row)
    rows.sort(key=lambda r: (r["path"].casefold(), r["path"]))
    groups = {}
    for row in rows:
        if row["payload_sha256"]:
            groups.setdefault(row["payload_sha256"], []).append(row)
    unique = []
    for digest, members in groups.items():
        members.sort(key=lambda r: (KIND_RANK.get(r["kind"], 3), len(r["path"]), r["path"]))
        primary, labels = members[0], sorted({m["classification"] for m in members})
        for member in members:
            member["dedup_group"], member["primary_path"] = digest[:16], primary["path"]
            member["is_duplicate"] = member is not primary
        unique.append({"payload_sha256": digest, "payload_size": primary["payload_size"],
                       "classification": primary["classification"], "labels": labels,
                       "primary_path": primary["path"], "primary_kind": primary["kind"],
                       "primary_entry": primary["entry"], "copies": len(members),
                       "members": [m["path"] for m in members], "language_verified": False})
    unique.sort(key=lambda u: (u["primary_path"].casefold(), u["primary_path"]))
    problems = [{"path": r["path"], "name": r["name"], "status": r["status"],
                 "classification": r["classification"], "flags": r["flags"],
                 "ambiguity": r["ambiguity"], "note": r["note"]}
                for r in rows if r["status"] not in CLEAN_STATUSES or r["ambiguity"] or r["flags"]]
    for record in unique:
        if len(record["labels"]) > 1:
            problems.append({"path": record["members"][0],
                             "name": os.path.basename(record["members"][0]),
                             "status": "label-conflict", "classification": "conflict",
                             "flags": [],
                             "ambiguity": "identical payload labelled %s" % ", ".join(record["labels"]),
                             "note": "payload %s" % record["payload_sha256"][:16]})
    counts = {"files": len(rows), "unique_payloads": len(unique),
              "duplicate_copies": sum(u["copies"] - 1 for u in unique),
              "label_conflicts": sum(1 for u in unique if len(u["labels"]) > 1),
              "problems": len(problems), "by_class": {}, "by_status": {}, "unique_by_class": {}}
    for key, field in (("by_class", "classification"), ("by_status", "status")):
        for row in rows:
            counts[key][row[field]] = counts[key].get(row[field], 0) + 1
    for record in unique:
        key = record["classification"]
        counts["unique_by_class"][key] = counts["unique_by_class"].get(key, 0) + 1
    return {"tool": TOOL, "schema": SCHEMA, "language_policy": POLICY,
            "scan": {"folder": os.path.abspath(folder), "recursive": recursive,
                     "extensions": list(CONTAINER_EXTS),
                     "hashes": "scripts/bench_catalog.py sha256_file/inspect_zip"},
            "counts": counts, "problems": problems, "unique": unique, "files": rows}


def render_summary(data):
    counts = data["counts"]

    def kv(mapping):
        return ", ".join("%s=%d" % (key, mapping[key]) for key in sorted(mapping))

    lines = ["korean-library: %s%s" % (data["scan"]["folder"],
                                        " (recursive)" if data["scan"]["recursive"] else ""),
             "files=%d  class: %s" % (counts["files"], kv(counts["by_class"])),
             "status: %s" % kv(counts["by_status"]),
             "unique payloads=%d  unique class: %s  duplicate copies=%d" % (
                 counts["unique_payloads"], kv(counts["unique_by_class"]),
                 counts["duplicate_copies"]),
             "",
             "Korea-tagged unique payloads (%d):" % counts["unique_by_class"].get("korea", 0)]
    korea = [u for u in data["unique"] if u["classification"] == "korea"]
    for index, record in enumerate(korea, 1):
        lines.append("  %2d. %-52s %s %10d B  copies=%d" % (
            index, os.path.basename(record["primary_path"])[:52],
            record["payload_sha256"][:16], record["payload_size"] or 0, record["copies"]))
    if not korea:
        lines.append("  (none)")
    bios = [r["name"] for r in data["files"] if r["classification"] == "bios"]
    if bios:
        lines.append("BIOS-labelled files (%d), not games: %s" % (len(bios), ", ".join(bios)))
    if data["problems"]:
        lines.append("problems (%d):" % len(data["problems"]))
        for problem in data["problems"]:
            lines.append("  %-40s %-16s %-9s %s" % (
                problem["name"][:40], problem["status"], problem["classification"],
                problem["ambiguity"] or problem["note"] or "flags=%s" % ",".join(problem["flags"])))
    else:
        lines.append("problems: none")
    lines.append("NOTE: " + POLICY)
    return "\n".join(lines) + "\n"


def output_guard(path, sources):
    """Refuse a report path that could overwrite a scanned file."""
    if ext(path) != ".json":
        return "error: --json-out must end in .json: %s" % path
    out = os.path.normcase(os.path.realpath(os.path.abspath(path)))
    for source in sources:
        same = os.path.normcase(os.path.realpath(source)) == out
        if not same and os.path.exists(path):
            try:
                same = os.path.samefile(path, source)
            except OSError:
                same = False
        if same:
            return "error: --json-out would overwrite a scanned file: %s" % source
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Read-only local GP32 Korean-library inventory; no network, no extraction.")
    parser.add_argument("folder", help="folder to inventory (top level only unless --recursive)")
    parser.add_argument("--recursive", action="store_true",
                        help="also scan subfolders (symlinks and junctions are skipped)")
    parser.add_argument("--json", action="store_true",
                        help="write only the deterministic JSON to stdout")
    parser.add_argument("--json-out", default=None,
                        help="also write the deterministic JSON to this file")
    args = parser.parse_args(argv)
    if not os.path.isdir(args.folder):
        print("error: not a folder: %s" % os.path.abspath(args.folder), file=sys.stderr)
        return 2
    files = collect_files(args.folder, args.recursive)
    if args.json_out:
        problem = output_guard(args.json_out, files)
        if problem:
            print(problem, file=sys.stderr)
            return 2
    data = build_inventory(args.folder, args.recursive, files)
    text = json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("wrote %s" % args.json_out, file=sys.stderr)
    sys.stdout.write(text if args.json else render_summary(data))
    return 1 if data["counts"]["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
