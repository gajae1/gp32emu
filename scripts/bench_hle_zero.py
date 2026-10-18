#!/usr/bin/env python3
"""Interleaved, affinity-pinned whole-span HLE zero comparison.

Uses equal work per variant, independent processes, full RAM verification,
and a target byte budget with minimum/maximum operation counts. Timing is
only inside the C binary; setup and final byte checks are excluded.
"""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import statistics
import subprocess


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("output")
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--target-bytes", type=int, default=64 * 1024 * 1024)
    args = parser.parse_args()
    if args.rounds < 1 or args.target_bytes < 1:
        parser.error("rounds and target-bytes must be positive")
    allowed = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    cpu = args.cpu if args.cpu is not None else (allowed[0] if allowed else None)
    if cpu is not None:
        os.sched_setaffinity(0, {cpu})
    bins = {"before": str(pathlib.Path(args.before).resolve()), "after": str(pathlib.Path(args.after).resolve())}
    report = {
        "schema": 1,
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": platform.platform(), "allowed_cpus": allowed, "pinned_cpu": cpu,
        "rounds": args.rounds, "target_bytes": args.target_bytes,
        "binaries": {key: {"path": path, "sha256": digest(path)} for key, path in bins.items()},
        "warmup_operations_per_sample": 64, "samples": [], "summary": []}
    for size in [4, 8, 16, 64, 256, 4096, 65536, 153600, 524288]:
        count = min(4_000_000, max(256, args.target_bytes // size))
        for working in [1, 8]:
            groups = {key: [] for key in bins}
            for round_ in range(args.rounds):
                order = ["before", "after"] if round_ % 2 == 0 else ["after", "before"]
                for key in order:
                    result = subprocess.run([bins[key], str(size), str(count), str(working)],
                                            check=True, capture_output=True, text=True)
                    row = json.loads(result.stdout)
                    if (row["bytes"], row["iterations"], row["working_spans"], row["guest_cycles"]) != (size, count, working, 0):
                        raise RuntimeError("benchmark returned mismatched work")
                    row.update(variant=key, round=round_, order=order.index(key))
                    groups[key].append(row["ns_per_zero"])
                    report["samples"].append(row)
            before, after = (statistics.median(groups[key]) for key in ["before", "after"])
            report["summary"].append({"bytes": size, "working_spans": working,
                                      "before_ns": before, "after_ns": after,
                                      "change_percent": (after / before - 1) * 100})
    report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    pathlib.Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    print(f"Wrote {len(report['samples'])} checked samples to {args.output}")


if __name__ == "__main__":
    main()
