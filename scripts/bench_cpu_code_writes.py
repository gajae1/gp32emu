#!/usr/bin/env python3
"""Interleave CPU store benchmark binaries and retain every checked sample."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--intermediate", type=Path, help="optional third binary, e.g. the rejected 64-byte prototype")
    parser.add_argument("--extended", action="store_true", help="bounded STR/STM16/4 KiB-stride comparison")
    parser.add_argument("--instructions", type=int, default=30_000_000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--cpu", type=int, help="Linux CPU affinity; defaults to first allowed CPU")
    args = parser.parse_args()
    multiple = 15 if args.extended else 3
    if args.instructions <= 0 or args.instructions % multiple or args.repetitions <= 0:
        parser.error(f"instructions must be a positive multiple of {multiple} and repetitions must be positive")
    cpu = args.cpu
    if hasattr(os, "sched_getaffinity"):
        allowed = os.sched_getaffinity(0)
        cpu = min(allowed) if cpu is None else cpu
        if cpu not in allowed:
            parser.error("requested CPU is outside this process's allowed affinity")
        os.sched_setaffinity(0, {cpu})
    elif cpu is not None:
        parser.error("explicit CPU affinity is unavailable on this platform")
    binaries = {"before": str(args.before.resolve())}
    if args.intermediate:
        binaries["intermediate"] = str(args.intermediate.resolve())
    binaries["after"] = str(args.after.resolve())
    result = {"cpu": cpu, "guest_instructions": args.instructions,
              "warmup_guest_instructions": 30000, "repetitions": args.repetitions,
              "extended": args.extended, "binaries": binaries, "results": []}
    if args.extended:
        cases = [(1, icache, mmu, placement, shape)
                 for shape in (0, 1, 2) for icache in (0, 1) for mmu in (0, 1)
                 for placement in ((0,) if shape == 2 else (0, 2))]
        cases += [(mode, 0, mmu, 0, 0) for mode in (0, 2) for mmu in (0, 1)]
    else:
        cases = [(mode, icache, mmu, placement, 0)
                 for mode in (1, 0, 2) for icache in (0, 1) for mmu in (0, 1)
                 for placement in ((0, 1, 2) if mode == 1 else (0,))]
    for mode, icache, mmu, placement, shape in cases:
        samples = {version: [] for version in binaries}
        versions = list(binaries)
        for run in range(args.repetitions):
            start = run % len(versions)
            order = versions[start:] + versions[:start]
            for version in order:
                command = [binaries[version], str(mode), str(icache), str(mmu),
                           str(args.instructions), str(placement), str(shape)]
                completed = subprocess.run(command, check=True, text=True, capture_output=True)
                match = re.search(r"ns_per_insn=([0-9.]+)", completed.stdout)
                if not match:
                    raise RuntimeError(f"missing timing in {version}: {completed.stdout}")
                samples[version].append({"ns_per_insn": float(match[1]),
                                         "output": completed.stdout.strip()})
        record = {"mode": mode, "I": icache, "MMU": mmu,
                  "placement": placement, "shape": shape, "samples": samples}
        for version in samples:
            record[version + "_median"] = statistics.median(s["ns_per_insn"] for s in samples[version])
        record["change_percent"] = (record["after_median"] / record["before_median"] - 1) * 100
        result["results"].append(record)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(mode, icache, mmu, placement, shape,
              {version: record[version + "_median"] for version in versions},
              round(record["change_percent"], 2), flush=True)


if __name__ == "__main__":
    main()
