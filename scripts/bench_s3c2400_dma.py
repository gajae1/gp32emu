#!/usr/bin/env python3
"""Interleave public-bus DMA benchmarks and retain every checked raw sample."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--configuration", required=True, help="compiler, flags, and library CPU-profile setting")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--target-bytes", type=int, default=134217728)
    parser.add_argument("--max-iterations", type=int, default=4000000)
    parser.add_argument("--min-iterations", type=int, default=256)
    parser.add_argument("--cpu", type=int, help="Linux CPU affinity; defaults to first allowed CPU")
    args = parser.parse_args()
    if min(args.repetitions, args.target_bytes, args.min_iterations) <= 0 or \
            args.max_iterations < args.min_iterations or args.max_iterations > 0xffffffff:
        parser.error("repetitions and target bytes must be positive, and 1 <= min <= max iterations <= UINT32_MAX")
    cpu = args.cpu
    if hasattr(os, "sched_getaffinity"):
        allowed = os.sched_getaffinity(0)
        cpu = min(allowed) if cpu is None else cpu
        if cpu not in allowed:
            parser.error("requested CPU is outside this process's allowed affinity")
        os.sched_setaffinity(0, {cpu})
    elif cpu is not None:
        parser.error("explicit CPU affinity is unavailable on this platform")
    binaries = {"before": str(args.before.resolve()), "after": str(args.after.resolve())}
    result = {"configuration": args.configuration, "cpu": cpu, "platform": platform.platform(),
              "clock": "CLOCK_MONOTONIC", "warmup_transfers": 64,
              "repetitions": args.repetitions, "target_bytes": args.target_bytes,
              "min_iterations": args.min_iterations, "max_iterations": args.max_iterations,
              "binaries": binaries, "results": []}
    for dsz in range(3):
        for size in (4, 8, 32, 4096, 153600, 524288):
            iterations = max(args.min_iterations, min(args.max_iterations, args.target_bytes // size))
            samples = {version: [] for version in binaries}
            hashes = set()
            for run in range(args.repetitions):
                order = ("before", "after") if run % 2 == 0 else ("after", "before")
                for version in order:
                    command = [binaries[version], str(dsz), str(size), str(iterations)]
                    start = time.time()
                    completed = subprocess.run(command, check=True, text=True, capture_output=True)
                    fields = dict(re.findall(r"(\w+)=([^\s]+)", completed.stdout))
                    if (int(fields["dsz"]), int(fields["bytes"]), int(fields["iterations"])) != \
                            (dsz, size, iterations):
                        raise RuntimeError(f"wrong benchmark case from {version}: {completed.stdout}")
                    hashes.add(fields["hash"])
                    samples[version].append({"ns_per_transfer": float(fields["ns_per_transfer"]),
                                             "start_epoch": start, "end_epoch": time.time(),
                                             "output": completed.stdout.strip()})
            if len(hashes) != 1:
                raise RuntimeError(f"payload hashes differ for DSZ {dsz}, {size} bytes: {hashes}")
            record = {"dsz": dsz, "bytes": size, "iterations": iterations, "samples": samples}
            for version in samples:
                record[version + "_median_ns"] = statistics.median(s["ns_per_transfer"] for s in samples[version])
            record["ratio"] = record["before_median_ns"] / record["after_median_ns"]
            result["results"].append(record)
            args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print(dsz, size, {version: record[version + "_median_ns"] for version in binaries},
                  round(record["ratio"], 3), flush=True)


if __name__ == "__main__":
    main()
