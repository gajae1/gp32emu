#!/usr/bin/env python3
"""Paired, pinned host HLE output cost with every RAM byte checked per sample.

No guest work is inside the timing region. Baseline and current snapshots must
use identical compilers and flags, with profiling disabled for timing. Setup,
64 warmup outputs and verification are excluded from elapsed_ns.
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
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("before")
    p.add_argument("after")
    p.add_argument("output")
    p.add_argument("--rounds", type=int, default=7)
    p.add_argument("--scalar-iterations", type=int, default=2000000)
    p.add_argument("--string-iterations", type=int, default=500000)
    p.add_argument("--cpu", type=int)
    p.add_argument("--verify", action="store_true")
    args = p.parse_args()
    if args.rounds < 1 or not all(1 <= n <= 1000000000 for n in
                                  [args.scalar_iterations, args.string_iterations]):
        p.error("positive rounds and 1..1000000000 iterations required")
    allowed = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    cpu = args.cpu if args.cpu is not None else allowed[0] if allowed else None
    if cpu is not None:
        os.sched_setaffinity(0, {cpu})
    bins = {"before": str(Path(args.before).resolve()), "after": str(Path(args.after).resolve())}
    report = {
        "schema": 1, "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": platform.platform(), "allowed_cpus": allowed, "pinned_cpu": cpu,
        "rounds": args.rounds, "clock_suppressed": args.verify,
        "scalar_iterations": args.scalar_iterations, "string_iterations": args.string_iterations,
        "pair_order": "AB, BA, AB, BA, ...", "warmup_calls_per_sample": 64,
        "writers": {"0": "word output", "1": "byte output", "2": "32-byte cstring",
                    "3": "256-byte cstring"},
        "placements": {"0": "data on different page", "1": "data on translated code page"},
        "binaries": {k: {"path": v, "sha256": hashlib.sha256(Path(v).read_bytes()).hexdigest()}
                     for k, v in bins.items()}, "samples": [], "summary": []}
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    for writer in range(4):
        count = args.scalar_iterations if writer < 2 else args.string_iterations
        for placement in range(2):
            for icache in range(2):
                values = {key: [] for key in bins}
                pairs = []
                for round_ in range(args.rounds):
                    order = ["before", "after"] if round_ % 2 == 0 else ["after", "before"]
                    pair = {}
                    for key in order:
                        cmd = [bins[key], str(writer), str(placement), str(icache),
                               str(count), str(int(not args.verify))]
                        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
                        row = json.loads(result.stdout)
                        if (row["writer"], row["placement"], row["icache"], row["iterations"],
                            row["guest_cycles"], row["verified_bytes"], row["bytes"]) != (
                                writer, placement, icache, count, 0, 8 * 1024 * 1024,
                                [4, 1, 32, 256][writer]):
                            raise RuntimeError("mismatched or unverified host work")
                        if args.verify:
                            if row["elapsed_ns"] or not row["profile_enabled"] or row["invalidations"]:
                                raise RuntimeError("non-code outputs changed translation epoch")
                        elif row["profile_enabled"] or not row["elapsed_ns"]:
                            raise RuntimeError("timing requires profiling off and a live clock")
                        row.update(variant=key, round=round_, order=order.index(key))
                        report["samples"].append(row)
                        values[key].append(row["ns_per_call"])
                        pair[key] = row["ns_per_call"]
                    if not args.verify:
                        pairs.append(100 * (pair["after"] / pair["before"] - 1))
                if not args.verify:
                    before = statistics.median(values["before"])
                    after = statistics.median(values["after"])
                    report["summary"].append({"writer": writer, "placement": placement, "icache": icache,
                        "before_ns": before, "after_ns": after, "added_ns": after - before,
                        "change_percent": 100 * (after / before - 1),
                        "paired_change_percent": pairs, "improved_pairs": sum(x < 0 for x in pairs)})
                output.write_text(json.dumps(report, indent=2) + "\n")
    report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Saved {len(report['samples'])} {'verification' if args.verify else 'timed'} samples: {output}")


if __name__ == "__main__":
    main()
