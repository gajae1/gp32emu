#!/usr/bin/env python3
"""Interleave two transfer-loop binaries on one permitted CPU and retain samples."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("before", type=Path)
parser.add_argument("after", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--instructions", type=int, default=3_000_000)
parser.add_argument("--repetitions", type=int, default=5)
parser.add_argument("--configuration", required=True)
args = parser.parse_args()
if args.instructions <= 0 or args.instructions % 5 or args.repetitions <= 0:
    parser.error("instructions must be a positive multiple of five and repetitions must be positive")
cpu = min(os.sched_getaffinity(0))
os.sched_setaffinity(0, {cpu})
binaries = {"before": str(args.before.resolve()), "after": str(args.after.resolve())}
result = {"cpu": cpu, "instructions": args.instructions, "warmup_instructions": 30_000,
          "repetitions": args.repetitions, "configuration": args.configuration,
          "binaries": binaries, "results": []}
for thumb in (1, 0):
    for mask in (0x01, 0x80, 0x81, 0x55, 0x7f, 0xff):
        samples = {version: [] for version in binaries}
        hashes = set()
        for repetition in range(args.repetitions):
            versions = ("after", "before") if repetition % 2 else ("before", "after")
            for version in versions:
                done = subprocess.run([binaries[version], str(thumb), f"{mask:x}", str(args.instructions)],
                                      check=True, capture_output=True, text=True)
                match = re.fullmatch(r"thumb=\d list=[0-9a-f]+ retired=\d+ ns_per_insn=([0-9.]+) hash=([0-9a-f]+) checked=1\n", done.stdout)
                if match is None:
                    raise RuntimeError(f"unchecked output: {done.stdout}")
                hashes.add(match[2])
                samples[version].append({"ns_per_insn": float(match[1]), "output": done.stdout.strip()})
        if len(hashes) != 1:
            raise RuntimeError(f"before/after state mismatch for Thumb={thumb} mask={mask:x}")
        record = {"thumb": thumb, "mask": f"{mask:02x}", "samples": samples}
        for version in binaries:
            record[version + "_median"] = statistics.median(s["ns_per_insn"] for s in samples[version])
        record["change_percent"] = (record["after_median"] / record["before_median"] - 1) * 100
        result["results"].append(record)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(thumb, f"{mask:02x}", record["before_median"], record["after_median"],
              round(record["change_percent"], 2), flush=True)
