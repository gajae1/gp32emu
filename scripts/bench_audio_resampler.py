#!/usr/bin/env python3
"""Compare current audio PCM/state and time against a named git revision.

Example (C23 dialect, identical optimization flags for both versions):
  python3 scripts/bench_audio_resampler.py --baseline <revision> --cc clang-18 \
      --cflags='-std=c23 -O3 -DNDEBUG' > audio.csv

Use --rounds 0 for correctness only, including with --runner='qemu-aarch64 -L
/usr/aarch64-linux-gnu'. QEMU timing is not device performance evidence.
The two revisions must use the same resampler state layout. The driver uses
a POSIX monotonic clock (Linux/macOS, or a suitable development environment).
"""

import argparse
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, help="git revision containing the reference source")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-std=c2x -O3 -DNDEBUG")
    parser.add_argument("--runner", default="")
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--frames", type=int, default=250000, help="approximate output frames per measurement")
    args = parser.parse_args()
    if args.rounds < 0 or args.frames <= 0:
        parser.error("rounds must be nonnegative and frames must be positive")
    root = Path(__file__).resolve().parents[1]
    source_path = "src/audio/gp32_audio_resampler.c"
    baseline = subprocess.check_output(
        ["git", "show", f"{args.baseline}:{source_path}"], cwd=root, text=True
    )
    functions = ["mark_gap_from_silence", "max_output_frames", "output_frames",
                 "mark_gap", "process", "reset", "init", "copy"]
    sources = {"baseline": baseline, "candidate": (root / source_path).read_text()}
    with tempfile.TemporaryDirectory(prefix="gp32-audio-bench-") as tmp:
        tmp = Path(tmp)
        paths = []
        for prefix, source in sources.items():
            for function in functions:
                source = source.replace(f"gp32_audio_resampler_{function}(", f"{prefix}_{function}(")
            path = tmp / f"{prefix}.c"
            path.write_text(source)
            paths.append(str(path))
        executable = tmp / "audio-compare"
        command = [args.cc, *shlex.split(args.cflags), "-I", str(root / "src"),
                   str(root / "scripts/audio_resampler_compare.c"), *paths,
                   "-lm", "-o", str(executable)]
        print("Build:", shlex.join(command), file=sys.stderr)
        subprocess.run(command, check=True)
        subprocess.run([*shlex.split(args.runner), str(executable),
                        str(args.rounds), str(args.frames)], check=True)


if __name__ == "__main__":
    main()
