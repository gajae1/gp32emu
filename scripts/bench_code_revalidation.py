#!/usr/bin/env python3
"""Paired isolated code-revalidation benchmark; never a game-FPS benchmark.

Build bench_code_revalidation.c twice with identical options, selecting the
baseline source through CORE_UNDER_TEST. Use GP32EMU_CPU_PROFILE=0 for release
timings and retain a separate profiling build to check actual backend counts.
"""
import argparse
import itertools
import json
import os
import pathlib
import random
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True)
    parser.add_argument("--after", required=True)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--rounds", type=int, default=1000000)
    parser.add_argument("--scope", choices=("release", "controls"), default="release")
    parser.add_argument("--icache", type=int, choices=(0, 1), default=1)
    parser.add_argument("--cpu", type=int)
    args = parser.parse_args()
    cpu = min(os.sched_getaffinity(0)) if args.cpu is None else args.cpu
    os.sched_setaffinity(0, {cpu})
    if args.samples < 1 or not 1 <= args.rounds <= 10000000:
        parser.error("samples and rounds must be positive; rounds must be <= 10000000")
    if args.scope == "release":
        conditions = list(itertools.product([1], [2, 16, 48, 96], [1, 64], [0, 1]))
        conditions += [(0, 2, 1, 1), (0, 2, 64, 1),
                       (0, 48, 64, 1), (0, 96, 64, 1), (0, 96, 64, 0)]
    else:
        conditions = list(itertools.product([1], [2, 48, 96], [64], [0, 1]))
        conditions += [(0, 2, 64, 1), (0, 96, 64, 0), (0, 96, 64, 1)]
    variants = [("before", args.before), ("after", args.after)]
    out = {"label": args.label, "cpu": cpu, "rounds": args.rounds,
           "samples_per_condition": args.samples, "scope": args.scope,
           "clock": "CLOCK_MONOTONIC", "icache": args.icache,
           "started_at_epoch": time.time(), "results": []}
    shuffle = random.Random(0x243F6A88)
    start = time.monotonic()
    for sample in range(args.samples):
        order = conditions.copy()
        shuffle.shuffle(order)
        for condition in order:
            for variant, binary in variants if sample % 2 == 0 else variants[::-1]:
                run = subprocess.run([binary, *map(str, condition), str(args.rounds), str(args.icache)],
                                     capture_output=True, text=True, check=True)
                data = {key: float(value) if "." in value else int(value)
                        for key, value in (part.split("=", 1) for part in run.stdout.split())}
                if data["ok"] != 1:
                    raise RuntimeError(data)
                out["results"].append({"sample": sample, "variant": variant, **data})
        out["finished_at_epoch"] = time.time()
        args.output.write_text(json.dumps(out, indent=2) + "\n")
        print(args.label, "sample", sample + 1, "seconds",
              round(time.monotonic() - start, 2), flush=True)
    keys = ("mode", "count", "working", "maintenance")
    for condition in conditions:
        rows = [row for row in out["results"] if tuple(row[key] for key in keys) == condition]
        med = {variant: statistics.median(row["ns_per_block"] for row in rows
                                         if row["variant"] == variant)
               for variant, _ in variants}
        print(condition, "before", round(med["before"], 3), "after", round(med["after"], 3),
              "change_percent", round(100 * (med["after"] / med["before"] - 1), 2), flush=True)


if __name__ == "__main__":
    main()
