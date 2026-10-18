#!/usr/bin/env python3
"""Compare equal external-write workloads in isolated, CPU-pinned processes.

Pass two binaries compiled from bench_external_writes.c with identical flags.
Seven pairs alternate AB/BA; setup, warmup, and full-RAM verification are not
timed. --verify suppresses clocks for profile-enabled native coverage checks.
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
    p.add_argument("--iterations", type=int, default=250000)
    p.add_argument("--cpu", type=int)
    p.add_argument("--verify", action="store_true")
    args = p.parse_args()
    if args.rounds < 1 or not 1 <= args.iterations <= 1000000000:
        p.error("positive rounds and 1..1000000000 iterations required")
    allowed = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    cpu = args.cpu if args.cpu is not None else allowed[0] if allowed else None
    if cpu is not None:
        os.sched_setaffinity(0, {cpu})
    bins = {"before": str(Path(args.before).resolve()), "after": str(Path(args.after).resolve())}
    report = {
        "schema": 1, "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": platform.platform(), "allowed_cpus": allowed, "pinned_cpu": cpu,
        "clock_suppressed": args.verify, "rounds": args.rounds, "iterations": args.iterations,
        "pair_order": "AB, BA, AB, BA, ...", "warmup_calls_per_sample": 64,
        "scenarios": {"0": "no-notification control", "1": "same-page adjacent data, 4 bytes",
                      "2": "same-page separated data, 128 bytes", "3": "data on another page, 4 bytes",
                      "4": "unchanged instruction word, 4 bytes"},
        "binaries": {k: {"path": v, "sha256": hashlib.sha256(Path(v).read_bytes()).hexdigest()}
                     for k, v in bins.items()}, "samples": [], "summary": []}
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    for native in [0, 1]:
        for icache in [0, 1]:
            for scenario in range(5):
                values = {key: [] for key in bins}
                pairs = []
                for round_ in range(args.rounds):
                    order = ["before", "after"] if round_ % 2 == 0 else ["after", "before"]
                    pair = {}
                    for key in order:
                        cmd = [bins[key], str(native), str(icache), str(scenario),
                               str(args.iterations), str(int(not args.verify))]
                        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
                        row = json.loads(result.stdout)
                        if (row["native"], row["icache"], row["scenario"], row["iterations"],
                            row["guest_cycles"], row["verified_bytes"]) != (
                                native, icache, scenario, args.iterations, args.iterations * 2, 8 * 1024 * 1024):
                            raise RuntimeError("mismatched or unverified guest work")
                        if args.verify:
                            if row["elapsed_ns"] or not row["profile_enabled"] or (
                                native and row["native_insns"] != row["guest_cycles"]):
                                raise RuntimeError("coverage check failed")
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
                    report["summary"].append({"native": native, "icache": icache, "scenario": scenario,
                        "before_ns": before, "after_ns": after, "change_percent": 100 * (after / before - 1),
                        "paired_change_percent": pairs, "improved_pairs": sum(x < 0 for x in pairs)})
                output.write_text(json.dumps(report, indent=2) + "\n")
    report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Saved {len(report['samples'])} {'verification' if args.verify else 'timed'} samples: {output}")


if __name__ == "__main__":
    main()
