#!/usr/bin/env python3
"""Build reproducible CPU external-write and host HLE output benchmark pairs.

Invoke once for each immutable source checkout, with separate output folders.
Both compilers get Release C23 builds with IPO off. Profiling-enabled binaries
are for clock-suppressed coverage only; timing uses the separate profile0 set.
The driver and independent fixture come from this script's repository, while
every production source/header comes from the requested source checkout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source")
    p.add_argument("output")
    p.add_argument("--cmake", default="cmake")
    p.add_argument("--gcc", default="gcc")
    p.add_argument("--clang", default="clang-18")
    p.add_argument("--jobs", type=int, default=2)
    args = p.parse_args()
    source, output = Path(args.source).resolve(), Path(args.output).resolve()
    drivers = Path(__file__).resolve().parents[1]
    if args.jobs < 1 or not (source / "src/arm920t.c").is_file():
        p.error("valid source checkout and positive jobs required")
    output.mkdir(parents=True, exist_ok=True)
    manifest = {
        "source": str(source), "driver_source": str(drivers),
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
        "source_status": subprocess.check_output(["git", "status", "--short"], cwd=source, text=True),
        "source_files": {str(f.relative_to(source)): sha(f)
                         for directory in [source / "src", source / "include"]
                         for f in sorted(directory.rglob("*")) if f.is_file()},
        "drivers": {name: sha(drivers / name) for name in [
            "scripts/bench_external_writes.c", "scripts/bench_hle_outputs.c",
            "tests/arm_external_write_test.c", "scripts/bench_external_writes.py",
            "scripts/bench_hle_outputs.py", "scripts/build_core_write_benchmarks.py"]},
        "builds": []}
    for label, compiler in [("gcc", args.gcc), ("clang", args.clang)]:
        cc = shutil.which(compiler)
        if not cc:
            raise RuntimeError(f"compiler unavailable: {compiler}")
        version = subprocess.check_output([cc, "--version"], text=True).splitlines()[0]
        for profile in [0, 1]:
            build = output / f"{label}-profile{profile}"
            build.mkdir(parents=True, exist_ok=True)
            configure = [args.cmake, "-S", str(source), "-B", str(build),
                f"-DCMAKE_C_COMPILER={cc}", "-DCMAKE_BUILD_TYPE=Release",
                "-DGP32EMU_REQUIRE_C23=ON", "-DGP32EMU_ENABLE_IPO=OFF",
                f"-DGP32EMU_CPU_PROFILE={'ON' if profile else 'OFF'}",
                "-DGP32EMU_BUILD_TESTS=OFF", "-DGP32EMU_BUILD_HEADLESS=OFF",
                "-DGP32EMU_BUILD_LIBRETRO=OFF", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
            compile_core = [args.cmake, "--build", str(build), "--target", "gp32emu", "-j", str(args.jobs)]
            flags = [cc, "-O3", "-DNDEBUG", "-std=c2x", "-fno-lto", "-D_XOPEN_SOURCE=700",
                "-D_FILE_OFFSET_BITS=64", "-DGP32EMU_ENABLE_THREADS=1",
                f"-DGP32EMU_CPU_PROFILE={profile}", "-I", str(source / "src"),
                "-I", str(source / "include")]
            external = flags + [f'-DARM_SOURCE="{source / "src/arm920t.c"}"',
                str(drivers / "scripts/bench_external_writes.c"), "-lm", "-o", str(build / "external-writes")]
            hle = flags + [f'-DGP32_CORE_UNDER_TEST="{source / "src/gp32.c"}"',
                str(drivers / "scripts/bench_hle_outputs.c"), str(build / "libgp32emu.a"),
                "-lm", "-pthread", "-o", str(build / "hle-outputs")]
            commands = [configure, compile_core, external, hle]
            with (build / "build.log").open("w") as log:
                for command in commands:
                    log.write(json.dumps(command) + "\n")
                    log.flush()
                    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            manifest["builds"].append({"compiler": label, "version": version, "profile": profile,
                "commands": commands, "log_sha256": sha(build / "build.log"),
                "core_compile_commands": json.loads((build / "compile_commands.json").read_text()),
                "binaries": {name: {"path": str(build / name), "sha256": sha(build / name)}
                             for name in ["external-writes", "hle-outputs"]}})
            (output / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            print(f"Built {label} profile{profile}: {build}", flush=True)


if __name__ == "__main__":
    main()
