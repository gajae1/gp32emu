#!/usr/bin/env python3
"""GP32 owned-ROM inventory and headless catalog benchmark runner (resume12).

Inventory-only by default: scans owned .smc/.fxe/.fpk/.zip files, hashes the ROM
payload, and deduplicates identical ROMs. Nothing under the scanned asset roots is
written or modified. ZIP handling streams exactly one .smc/.fxe/.fpk entry into a
unique folder under the results work directory; entry names are reduced to a safe
basename and traversal entries are refused, so archive contents cannot escape it.

BENCHMARK CAVEAT: with the frozen runner and no per-title state/input script, the
measured window is a boot/menu/attract window (default 2400 warmup + 300 frames,
reported as scene_kind boot-menu-<frames>f) with the runner autopulse input. That
is a throughput smoke metric. It is NOT gameplay, it is NOT a pass/fail gate, and
every benchmark record carries gameplay_measured=false and
manual_gameplay_pending=true.

Defaults are derived from this file's location: the repository parent directory is
the default asset root (top-level only) with its test-assets/ subdirectory scanned
recursively, and results/ under the same parent is the default output directory.
Benchmarking requires explicit --benchmark and --bios paths.

Usage:
  python scripts/bench_catalog.py
  python scripts/bench_catalog.py --execute --benchmark <runner.exe> --bios <bios.bin>
  python scripts/bench_catalog.py --execute --only oneshot --limit 1 --benchmark <runner.exe> --bios <bios.bin>
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROM_EXTS = (".smc", ".fxe", ".fpk")
CONTAINER_EXTS = ROM_EXTS + (".zip",)
SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parent.parent
ASSET_PARENT = REPO_ROOT.parent
DEFAULT_ROOTS = (str(ASSET_PARENT), str(ASSET_PARENT / "test-assets"))
DEFAULT_RESULTS = str(ASSET_PARENT / "results")
DEFAULT_PREFIX = "resume12-catalog"
CHUNK = 4 * 1024 * 1024
SAFE = re.compile(r"[^A-Za-z0-9._()&',+ -]+")


def bench_note(warmup, frames):
    return ("%d warmup + %d measured frames with autopulse input is a boot/menu/"
            "attract window, not gameplay and not a pass/fail gate." % (warmup, frames))


def scene_kind_for(frames):
    return "boot-menu-%df" % frames


def utc_now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def ext_of(name):
    return os.path.splitext(name)[1].lower()


def safe_basename(name):
    base = name.replace("\\", "/").split("/")[-1]
    base = SAFE.sub("_", base).strip(" .")
    return base or "rom.bin"


def is_unsafe_entry(name):
    norm = name.replace("\\", "/")
    parts = norm.split("/")
    return norm.startswith("/") or norm.startswith("\\") or ".." in parts or ":" in norm


def sha256_file(path, budget):
    h = hashlib.sha256()
    total = 0
    start = time.monotonic()
    with open(path, "rb") as fh:
        while True:
            if budget and time.monotonic() - start > budget:
                raise TimeoutError("hash budget %ss exceeded" % budget)
            chunk = fh.read(CHUNK)
            if not chunk:
                break
            h.update(chunk)
            total += len(chunk)
    return h.hexdigest(), total


def inspect_zip(path, max_entry_bytes, max_ratio, budget):
    out = {"status": "ok", "entries": [], "entry": None, "payload_sha256": None,
           "payload_size": None, "note": None}
    file_size = os.path.getsize(path)
    try:
        zf = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as exc:
        out["status"] = "zip-read-error"
        out["note"] = str(exc)
        return out
    with zf:
        candidates = []
        for zi in zf.infolist():
            if zi.is_dir():
                continue
            unsafe = is_unsafe_entry(zi.filename)
            rom = ext_of(zi.filename) in ROM_EXTS
            out["entries"].append({"name": zi.filename, "size": zi.file_size,
                                   "compressed": zi.compress_size, "rom": rom,
                                   "unsafe": unsafe})
            if rom and not unsafe and zi.filename not in (o["name"] for o in out["entries"][:-1]):
                candidates.append(zi)
        if not candidates:
            out["status"] = "no-rom-entry"
            return out
        if len(candidates) > 1:
            out["status"] = "ambiguous-zip"
            out["note"] = "multiple ROM entries; not auto-selected"
            return out
        zi = candidates[0]
        out["entry"] = zi.filename
        if zi.file_size > max_entry_bytes:
            out["status"] = "entry-too-large"
            out["note"] = "%d bytes exceeds cap %d" % (zi.file_size, max_entry_bytes)
            return out
        if file_size > 0 and zi.file_size / file_size > max_ratio:
            out["status"] = "zip-ratio-suspect"
            out["note"] = "declared ratio %.1f exceeds %.1f" % (zi.file_size / file_size, max_ratio)
            return out
        h = hashlib.sha256()
        total = 0
        start = time.monotonic()
        try:
            with zf.open(zi) as src:
                while True:
                    if budget and time.monotonic() - start > budget:
                        out["status"] = "zip-read-timeout"
                        out["note"] = "stream budget %ss exceeded" % budget
                        return out
                    chunk = src.read(CHUNK)
                    if not chunk:
                        break
                    h.update(chunk)
                    total += len(chunk)
        except (zipfile.BadZipFile, OSError, RuntimeError) as exc:
            out["status"] = "zip-read-error"
            out["note"] = str(exc)
            return out
        if total != zi.file_size:
            out["status"] = "zip-read-error"
            out["note"] = "streamed %d != declared %d" % (total, zi.file_size)
            return out
        out["payload_sha256"] = h.hexdigest()
        out["payload_size"] = total
    return out


def collect_files(roots):
    files = []
    for index, root in enumerate(roots):
        root = os.path.abspath(root)
        if not os.path.isdir(root):
            continue
        if index == 0:
            for name in sorted(os.listdir(root)):
                full = os.path.join(root, name)
                if os.path.isfile(full) and ext_of(name) in CONTAINER_EXTS:
                    files.append(full)
        else:
            for base, dirs, names in os.walk(root):
                dirs[:] = [d for d in sorted(dirs) if not d.startswith(".")]
                for name in sorted(names):
                    if ext_of(name) in CONTAINER_EXTS:
                        files.append(os.path.join(base, name))
    return files


def build_inventory(args):
    rows = []
    deadline = time.monotonic() + args.total_timeout if args.total_timeout else None
    for path in collect_files(args.roots):
        row = {"path": path, "name": os.path.basename(path), "kind": "zip" if ext_of(path) == ".zip" else "rom",
               "size": os.path.getsize(path), "sha256": None, "payload_sha256": None,
               "payload_size": None, "entry": None, "status": "ok", "note": None}
        if deadline and time.monotonic() > deadline:
            row["status"] = "skipped-total-timeout"
            rows.append(row)
            continue
        try:
            row["sha256"], _ = sha256_file(path, args.zip_budget)
            if row["kind"] == "rom":
                row["payload_sha256"] = row["sha256"]
                row["payload_size"] = row["size"]
            else:
                info = inspect_zip(path, args.max_entry_bytes, args.max_zip_ratio, args.zip_budget)
                row["status"] = info["status"]
                row["entry"] = info["entry"]
                row["note"] = info["note"]
                row["payload_sha256"] = info["payload_sha256"]
                row["payload_size"] = info["payload_size"]
        except (OSError, TimeoutError) as exc:
            row["status"] = "unreadable" if isinstance(exc, OSError) else "hash-timeout"
            row["note"] = str(exc)
        if not args.quiet:
            print("scan %-9s %s" % (row["status"], row["name"]))
        rows.append(row)
    groups = {}
    for row in rows:
        if row["payload_sha256"]:
            groups.setdefault(row["payload_sha256"], []).append(row)
    for digest, members in groups.items():
        members.sort(key=lambda r: (0 if r["kind"] == "rom" else 1, len(r["path"]), r["path"]))
        primary = members[0]
        for row in members:
            row["dedup_group"] = digest[:16]
            row["primary_path"] = primary["path"]
            row["is_duplicate"] = row is not primary
    unique = []
    for digest, members in sorted(groups.items(), key=lambda kv: kv[1][0]["path"].lower()):
        primary = members[0]
        unique.append({"payload_sha256": digest, "payload_size": primary["payload_size"],
                       "primary_path": primary["path"], "primary_kind": primary["kind"],
                       "primary_entry": primary["entry"], "copies": len(members),
                       "members": [m["path"] for m in members]})
    data = {"generated": utc_now(), "roots": [os.path.abspath(r) for r in args.roots],
            "extensions": list(CONTAINER_EXTS), "files": rows, "unique_roms": unique,
            "counts": {"files": len(rows), "unique_roms": len(unique),
                       "duplicate_copies": sum(len(u["members"]) - 1 for u in unique),
                       "by_status": {}}}
    for row in rows:
        key = row["status"]
        data["counts"]["by_status"][key] = data["counts"]["by_status"].get(key, 0) + 1
    return data


def inventory_markdown(data, prefix):
    lines = ["# GP32 local game matrix (inventory)", "",
             "Generated %s by scripts/bench_catalog.py (inventory-only; no asset was modified)." % data["generated"],
             "", "Roots: %s" % ", ".join(data["roots"]), "",
             "| Metric | Count |", "| --- | --- |"]
    for key in ("files", "unique_roms", "duplicate_copies"):
        lines.append("| %s | %s |" % (key, data["counts"][key]))
    for key, value in sorted(data["counts"]["by_status"].items()):
        lines.append("| status: %s | %s |" % (key, value))
    lines += ["", "## Unique ROMs (deduplicated by payload SHA-256)", "",
              "| # | Payload SHA-256 | Bytes | Primary file | Container | Copies |",
              "| --- | --- | --- | --- | --- | --- |"]
    for i, rom in enumerate(data["unique_roms"], 1):
        lines.append("| %d | %s | %d | %s | %s | %d |" % (
            i, rom["payload_sha256"][:16], rom["payload_size"] or 0,
            os.path.basename(rom["primary_path"]), rom["primary_kind"], rom["copies"]))
    lines += ["", "## All scanned files", "",
              "| File | Kind | Bytes | Status | Entry | Note |", "| --- | --- | --- | --- | --- | --- |"]
    for row in data["files"]:
        lines.append("| %s | %s | %d | %s | %s | %s |" % (
            row["name"], row["kind"], row["size"], row["status"], row["entry"] or "", row["note"] or ""))
    return "\n".join(lines) + "\n"


def ensure_payload(row, extract_root, args):
    path = row["path"]
    if row["kind"] == "rom":
        return path
    name = safe_basename(row["entry"])
    dest_dir = os.path.join(extract_root, row["payload_sha256"][:16])
    dest = os.path.join(dest_dir, name)
    root_real = os.path.realpath(extract_root)
    dest_real = os.path.realpath(dest)
    if os.path.commonpath([root_real, dest_real]) != root_real:
        raise RuntimeError("refusing extraction outside %s" % extract_root)
    if os.path.isfile(dest) and os.path.getsize(dest) == row["payload_size"]:
        digest, _ = sha256_file(dest, args.zip_budget)
        if digest == row["payload_sha256"]:
            return dest
    os.makedirs(dest_dir, exist_ok=True)
    tmp = dest + ".part"
    h = hashlib.sha256()
    with zipfile.ZipFile(path) as zf, zf.open(row["entry"]) as src, open(tmp, "wb") as out:
        while True:
            chunk = src.read(CHUNK)
            if not chunk:
                break
            h.update(chunk)
            out.write(chunk)
    if h.hexdigest() != row["payload_sha256"]:
        os.remove(tmp)
        raise RuntimeError("streamed payload hash mismatch for %s" % path)
    os.replace(tmp, dest)
    return dest


def parse_metrics(stdout):
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                continue
    return None


def run_benchmark(unique_roms, rows_by_primary, args):
    extract_root = os.path.join(args.results_dir, args.prefix + "-extract")
    note = bench_note(args.warmup, args.frames)
    scene_kind = scene_kind_for(args.frames)
    results = []
    deadline = time.monotonic() + args.total_timeout if args.total_timeout else None
    selected = unique_roms
    if args.only:
        selected = [u for u in selected
                    if any(needle.lower() in u["primary_path"].lower() for needle in args.only)]
    if args.limit:
        selected = selected[:args.limit]
    for rom in selected:
        row = rows_by_primary[rom["primary_path"]]
        record = {"payload_sha256": rom["payload_sha256"], "container": row["path"],
                  "status": "pending", "exit_code": None, "duration_s": None,
                  "scene_kind": scene_kind, "gameplay_measured": False,
                  "manual_gameplay_pending": True, "note": note, "metrics": None}
        if deadline and time.monotonic() > deadline:
            record["status"] = "skipped-total-timeout"
            results.append(record)
            continue
        try:
            smc = ensure_payload(row, extract_root, args)
        except (OSError, RuntimeError, zipfile.BadZipFile, TimeoutError) as exc:
            record["status"] = "extract-error"
            record["note"] = "%s; %s" % (note, exc)
            results.append(record)
            continue
        record["smc"] = smc
        cmd = [args.benchmark, "--bios", args.bios, "--smc", smc,
               "--warmup", str(args.warmup), "--frames", str(args.frames)]
        if args.jit:
            cmd.append("--jit")
        if args.state:
            cmd += ["--state", args.state]
        if args.input_script:
            cmd += ["--input-script", args.input_script]
        if args.cpu_profile:
            cmd.append("--cpu-profile")
        start = time.monotonic()
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            record["status"] = "timeout"
            record["duration_s"] = round(time.monotonic() - start, 3)
            record["note"] = "%s; runner exceeded %ss" % (note, args.timeout)
            results.append(record)
            continue
        except (OSError, ValueError) as exc:
            record["status"] = "runner-error"
            record["note"] = "%s; %s" % (note, exc)
            results.append(record)
            continue
        record["duration_s"] = round(time.monotonic() - start, 3)
        record["exit_code"] = proc.returncode
        metrics = parse_metrics(proc.stdout)
        if metrics:
            record["status"] = "metric"
            record["metrics"] = metrics
        elif proc.returncode != 0:
            record["status"] = "error-exit"
            record["note"] = "%s; exit code %s" % (note, proc.returncode)
        else:
            record["status"] = "no-json"
            record["note"] = note
        record["stdout_tail"] = "\n".join(proc.stdout.strip().splitlines()[-3:])
        record["stderr_tail"] = "\n".join(proc.stderr.strip().splitlines()[-3:])
        if not args.quiet:
            print("bench %-9s %s" % (record["status"], os.path.basename(row["path"])))
        results.append(record)
    return results


def bench_markdown(data, prefix):
    lines = ["# GP32 local game matrix (benchmark)", "",
             "Generated %s by scripts/bench_catalog.py --execute (frozen runner: %s)." % (data["generated"], data["benchmark"]),
             "", "WARNING: every row below is a %s" % data["note"],
             "gameplay_measured is false for all rows; manual gameplay with a state and input script is pending.",
             "", "| # | ROM | Status | fps | Exit | Duration s | Scene | Manual gameplay |",
             "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for i, rec in enumerate(data["results"], 1):
        fps = ""
        if rec.get("metrics"):
            fps = "%s" % rec["metrics"].get("fps")
        lines.append("| %d | %s | %s | %s | %s | %s | %s | %s |" % (
            i, os.path.basename(rec["container"]), rec["status"], fps,
            rec["exit_code"] if rec["exit_code"] is not None else "",
            rec["duration_s"] if rec["duration_s"] is not None else "",
            rec["scene_kind"], "pending" if rec["manual_gameplay_pending"] else "done"))
    lines += ["", "Container: %s" % prefix]
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description="GP32 owned-ROM inventory; inventory-only unless --execute.")
    parser.add_argument("--root", dest="roots", action="append", default=None,
                        help="scan root; the first root is scanned top-level only, the rest recursively (default: repository parent dir, then its test-assets subdir)")
    parser.add_argument("--results-dir", default=DEFAULT_RESULTS)
    parser.add_argument("--prefix", default=DEFAULT_PREFIX)
    parser.add_argument("--execute", action="store_true", help="run the frozen benchmark (requires --benchmark and --bios)")
    parser.add_argument("--benchmark", default=None, help="frozen runner executable (required with --execute)")
    parser.add_argument("--bios", default=None, help="BIOS image (required with --execute)")
    parser.add_argument("--warmup", type=int, default=2400)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--no-jit", dest="jit", action="store_false", default=True)
    parser.add_argument("--state", default=None, help="per-run state override; only valid with a single selected ROM")
    parser.add_argument("--input-script", dest="input_script", default=None)
    parser.add_argument("--cpu-profile", dest="cpu_profile", action="store_true")
    parser.add_argument("--only", action="append", default=None, help="substring filter for ROM paths (repeatable)")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=600, help="per-run runner timeout in seconds")
    parser.add_argument("--total-timeout", type=int, default=3600, help="whole-pass timeout in seconds (0 disables)")
    parser.add_argument("--zip-budget", type=int, default=120, help="per-file hash/stream budget in seconds")
    parser.add_argument("--max-entry-bytes", type=int, default=128 * 1024 * 1024)
    parser.add_argument("--max-zip-ratio", type=float, default=200.0)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    args.roots = tuple(args.roots) if args.roots else DEFAULT_ROOTS
    if args.execute and not args.benchmark:
        print("--execute requires --benchmark <path>", file=sys.stderr)
        return 2
    if args.execute and not args.bios:
        print("--execute requires --bios <path>", file=sys.stderr)
        return 2
    os.makedirs(args.results_dir, exist_ok=True)
    data = build_inventory(args)
    inv_json = os.path.join(args.results_dir, args.prefix + "-inventory.json")
    inv_md = os.path.join(args.results_dir, args.prefix + "-inventory.md")
    with open(inv_json, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)
    with open(inv_md, "w", encoding="utf-8") as fh:
        fh.write(inventory_markdown(data, args.prefix))
    print("wrote %s (%d files, %d unique ROMs)" % (inv_json, data["counts"]["files"], data["counts"]["unique_roms"]))
    if not args.execute:
        return 0
    if not os.path.isfile(args.benchmark):
        print("benchmark binary not found: %s" % args.benchmark, file=sys.stderr)
        return 2
    if not os.path.isfile(args.bios):
        print("bios not found: %s" % args.bios, file=sys.stderr)
        return 2
    if args.state and args.limit != 1:
        print("--state requires --limit 1 so the state matches the selected ROM", file=sys.stderr)
        return 2
    rows_by_primary = {row["path"]: row for row in data["files"]}
    roms = [u for u in data["unique_roms"] if u["payload_sha256"]]
    selected_count = len(roms) if not args.limit else min(args.limit, len(roms))
    bench = {"generated": utc_now(), "benchmark": os.path.abspath(args.benchmark),
             "benchmark_sha256": sha256_file(args.benchmark, 600)[0],
             "bios": os.path.abspath(args.bios), "bios_sha256": sha256_file(args.bios, 600)[0],
             "warmup": args.warmup, "frames": args.frames, "jit": args.jit,
             "scene_kind": scene_kind_for(args.frames), "gameplay_measured": False,
             "manual_gameplay_pending": True, "note": bench_note(args.warmup, args.frames),
             "selected": selected_count, "results": run_benchmark(roms, rows_by_primary, args)}
    bench_json = os.path.join(args.results_dir, args.prefix + "-bench.json")
    bench_md = os.path.join(args.results_dir, args.prefix + "-bench.md")
    with open(bench_json, "w", encoding="utf-8") as fh:
        json.dump(bench, fh, ensure_ascii=False, indent=2)
    with open(bench_md, "w", encoding="utf-8") as fh:
        fh.write(bench_markdown(bench, args.prefix))
    measured = sum(1 for r in bench["results"] if r["status"] == "metric")
    print("wrote %s (%d/%d runs produced metrics)" % (bench_json, measured, len(bench["results"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
