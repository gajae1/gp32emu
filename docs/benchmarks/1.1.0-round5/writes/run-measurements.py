#!/usr/bin/env python3
"""Run the four frozen component comparisons sequentially; retain all samples."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("before_build")
    parser.add_argument("after_build")
    parser.add_argument("output")
    args = parser.parse_args()
    source = Path(args.source).resolve()
    before = Path(args.before_build).resolve()
    after = Path(args.after_build).resolve()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    cpuinfo = Path("/proc/cpuinfo").read_text()
    model = next(line.split(":", 1)[1].strip() for line in cpuinfo.splitlines()
                 if line.startswith("model name"))
    record = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": dict(platform.uname()._asdict()), "cpu_model": model,
        "allowed_cpus": sorted(os.sched_getaffinity(0)),
        "sequential_measurements": True,
        "context": "All builds, sanitizer tests, and functional/WASM executions finished before this window. "
                   "Agents continued lightweight documentation work. Other activity on the shared host was not controlled.",
        "commands": [],
    }
    manifest = output / "measurement-window.json"
    for component in ["external", "hle"]:
        for compiler in ["gcc", "clang"]:
            binary = "external-writes" if component == "external" else "hle-outputs"
            script = "bench_external_writes.py" if component == "external" else "bench_hle_outputs.py"
            command = [sys.executable, str(source / "scripts" / script),
                       str(before / (compiler + "-profile0") / binary),
                       str(after / (compiler + "-profile0") / binary),
                       str(output / (component + "-" + compiler + "-timed.json")),
                       "--rounds", "7"]
            command += (["--iterations", "250000"] if component == "external" else
                        ["--scalar-iterations", "2000000", "--string-iterations", "500000"])
            row = {"component": component, "compiler": compiler, "command": command,
                   "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
            record["commands"].append(row)
            manifest.write_text(json.dumps(record, indent=2) + "\n")
            with (output / (component + "-" + compiler + "-timed.log")).open("w") as log:
                code = subprocess.call(command, cwd=source, stdout=log, stderr=subprocess.STDOUT)
            row.update(returncode=code, finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
            manifest.write_text(json.dumps(record, indent=2) + "\n")
            print(component, compiler, "exit=" + str(code), flush=True)
            if code:
                raise SystemExit(code)
    record["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    manifest.write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
