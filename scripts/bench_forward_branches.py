#!/usr/bin/env python3
"""Interleave checked forward-branch timing pairs and retain every sample."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def cases():
    result = [(1, mmu, zeros, 2, 1) for mmu in (0, 1) for zeros in (0, 64, 128, 256)]
    result += [(1, 0, 128, width, 1) for width in (1, 4, 8)]
    result += [(1, mmu, 0, 2, 0) for mmu in (0, 1)]
    result += [(mode, 0, 128, 2, 1) for mode in (0, 2)]
    result += [(3, 0, 0, 2, forward) for forward in (0, 1)]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--pixels", type=int, default=32768)
    parser.add_argument("--passes", type=int, default=64)
    parser.add_argument("--compile-passes", type=int, default=5000)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--verify", action="store_true", help="check outputs and code sizes without reading the clock")
    args = parser.parse_args()
    if not 16 <= args.pixels <= 65536 or not 1 <= args.passes <= 1000000 or not 1 <= args.compile_passes <= 1000000 or args.repetitions < 1:
        parser.error("invalid pixel, pass or repetition count")
    cpu = args.cpu
    if hasattr(os, "sched_getaffinity"):
        allowed = os.sched_getaffinity(0)
        cpu = min(allowed) if cpu is None else cpu
        if cpu not in allowed:
            parser.error("requested CPU is outside allowed affinity")
        os.sched_setaffinity(0, {cpu})
    elif cpu is not None:
        parser.error("explicit CPU affinity is unavailable")
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    result = {"cpu": cpu, "platform": platform.platform(), "machine": platform.machine(),
              "pixels": args.pixels, "passes": args.passes, "compile_passes": args.compile_passes,
              "repetitions": args.repetitions, "warmup_passes": 2, "verify_only": args.verify,
              "binaries": {key: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                           for key, path in binaries.items()}, "results": []}
    for mode, mmu, zeros, width, forward in cases():
        samples = {"before": [], "after": []}
        expected = None
        for repetition in range(args.repetitions):
            order = ("before", "after") if repetition % 2 == 0 else ("after", "before")
            for version in order:
                command = [str(binaries[version]), str(mode), str(mmu), str(zeros), str(width),
                           str(forward), str(args.pixels),
                           str(args.compile_passes if mode == 3 else args.passes), str(int(args.verify))]
                completed = subprocess.run(command, check=True, capture_output=True, text=True)
                sample = json.loads(completed.stdout)
                if not sample["ok"] or (args.verify and sample["elapsed_ns"] != 0):
                    raise RuntimeError(f"invalid sample: {sample}")
                outputs = (sample["retired"], sample["checksum"], sample["unit"])
                if expected is None:
                    expected = outputs
                elif outputs != expected:
                    raise RuntimeError(f"architectural output differs: {outputs} != {expected}")
                samples[version].append(sample)
        if not forward and samples["before"][0]["code_bytes"] != samples["after"][0]["code_bytes"]:
            raise RuntimeError("no-forward control changed generated code size")
        record = {"mode": mode, "mmu": mmu, "zeros": zeros, "width": width, "forward": forward,
                  "samples": samples}
        for version in samples:
            record[version + "_median"] = statistics.median(s["ns_per_unit"] for s in samples[version])
        record["change_percent"] = None if args.verify else (record["after_median"] / record["before_median"] - 1) * 100
        result["results"].append(record)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(mode, mmu, zeros, width, forward, record["before_median"], record["after_median"],
              record["change_percent"], flush=True)


if __name__ == "__main__":
    main()
