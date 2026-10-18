#!/usr/bin/env python3
"""Run neighboring before/after predicate samples on one permitted CPU.

Compile each input binary from scripts/bench_native_predicates.c first. Keep
other builds/tests idle during measured runs. --verify disables the C clock
and uses tiny runs for every case, including cached/raw/compile controls.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", default="predicate-study")
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--elements", type=int, default=32768)
    parser.add_argument("--passes", type=int, default=64)
    parser.add_argument("--compile-passes", type=int, default=1000)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    if min(args.rounds, args.elements, args.passes, args.compile_passes) < 1:
        parser.error("counts must be positive")
    allowed = sorted(os.sched_getaffinity(0))
    cpu = allowed[0] if args.cpu is None else args.cpu
    if cpu not in allowed:
        parser.error("CPU is outside the permitted affinity set")
    os.sched_setaffinity(0, {cpu})
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    cases = [(1, cond, pattern) for cond in (0, 8, 9, 10, 11, 12, 13)
             for pattern in range(3)] + [(1, 14, 1)]
    cases += [(mode, 12, pattern) for mode in (0, 2) for pattern in range(3)]
    cases += [(mode, 14, 1) for mode in (0, 2)]
    cases += [(3, cond, 1) for cond in (0, 8, 9, 10, 11, 12, 13, 14)]
    data = {
        "schema_version": 1, "label": args.label, "verify_only": args.verify,
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": platform.uname()._asdict(), "cpu": cpu,
        "binary_sha256": {k: hashlib.sha256(p.read_bytes()).hexdigest() for k, p in binaries.items()},
        "binaries": {k: str(v) for k, v in binaries.items()}, "results": [],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        for repeat in range(1 if args.verify else args.rounds):
            # Rotate case order and alternate which member of each pair runs
            # first; each before/after pair remains immediately neighboring.
            offset = repeat % len(cases)
            ordered = cases[offset:] + cases[:offset]
            for index, (mode, cond, pattern) in enumerate(ordered):
                elements = 256 if args.verify else args.elements
                passes = 2 if args.verify else args.compile_passes if mode == 3 else args.passes
                variants = ("before", "after") if (repeat + index) % 2 == 0 else ("after", "before")
                pair = []
                for variant in variants:
                    command = [str(binaries[variant]), str(mode), str(cond), str(pattern),
                               str(elements), str(passes), str(int(args.verify))]
                    result = subprocess.run(command, text=True, capture_output=True, check=True)
                    row = json.loads(result.stdout)
                    if not row["ok"]:
                        raise RuntimeError(f"failed result checks: {command}: {row}")
                    row.update(variant=variant, repeat=repeat, command=command)
                    data["results"].append(row)
                    pair.append(row)
                for field in ("retired", "taken", "checksum", "unit"):
                    if pair[0][field] != pair[1][field]:
                        raise RuntimeError(f"before/after mismatch in {field}: {pair}")
            print(f"completed round {repeat + 1}, rows={len(data['results'])}", flush=True)
        data["complete"] = True
        if not args.verify:
            data["summary"] = []
            for mode, cond, pattern in cases:
                subset = [r for r in data["results"] if
                          (r["mode"], r["cond"], r["pattern"]) == (mode, cond, pattern)]
                samples = {variant: [r["ns_per_unit"] for r in subset if r["variant"] == variant]
                           for variant in binaries}
                medians = {k: statistics.median(v) for k, v in samples.items()}
                data["summary"].append({"mode": mode, "cond": cond, "pattern": pattern,
                    "unit": subset[0]["unit"], "medians": medians,
                    "change_percent": (medians["after"] / medians["before"] - 1) * 100,
                    "ranges": {k: [min(v), max(v)] for k, v in samples.items()}})
    finally:
        data["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        args.output.write_text(json.dumps(data, indent=2) + "\n")


if __name__ == "__main__":
    main()
