#!/usr/bin/env python3
"""Capture a fresh C23 core verification matrix, preserving failed evidence.

The checked-out production source and tests must match --frozen-head. Compiler
and sanitizer runs are functional gates, never performance measurements.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source")
    p.add_argument("build")
    p.add_argument("--frozen-head", required=True)
    p.add_argument("--kind", choices=["gcc", "clang", "aarch64", "ubsan"], required=True)
    p.add_argument("--cmake", default="cmake")
    p.add_argument("--expected-tests", type=int, default=64)
    p.add_argument("--jobs", type=int, default=2)
    args = p.parse_args()
    source, build = Path(args.source).resolve(), Path(args.build).resolve()
    if build.exists():
        p.error("use a new build directory to preserve earlier evidence")
    compiler = shutil.which({"gcc": "gcc", "clang": "clang-18", "aarch64": "aarch64-linux-gnu-gcc",
                             "ubsan": "gcc"}[args.kind])
    if not compiler:
        p.error("requested compiler is unavailable")
    cmake = str(Path(shutil.which(args.cmake) or args.cmake).resolve())
    ctest = str(Path(cmake).with_name("ctest"))

    def git(*argv):
        return subprocess.check_output(["git", *argv], cwd=source, text=True).strip()

    if git("rev-parse", "HEAD") != args.frozen_head:
        p.error("HEAD does not match the requested source freeze")
    source_paths = [path for path in git("ls-files").splitlines() if path.startswith(
        ("src/", "include/", "tests/", "ios/", "wasm/")) or path in ["CMakeLists.txt", "Makefile.wasm"]]
    hashes = {name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in source_paths}
    for name in source_paths:
        expected = subprocess.check_output(["git", "show", args.frozen_head + ":" + name], cwd=source)
        if hashlib.sha256(expected).hexdigest() != hashes[name]:
            p.error("source file differs from freeze: " + name)
    evidence = build / "verification"
    evidence.mkdir(parents=True)
    shutil.copyfile(__file__, evidence / "runner.py")
    configure = [cmake, "-S", str(source), "-B", str(build),
        "-DCMAKE_C_COMPILER=" + compiler, "-DCMAKE_BUILD_TYPE=" + ("RelWithDebInfo" if args.kind == "ubsan" else "Release"),
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DGP32EMU_REQUIRE_C23=ON",
        "-DGP32EMU_BUILD_TESTS=ON", "-DGP32EMU_BUILD_LIBRETRO=ON", "-DGP32EMU_BUILD_HEADLESS=ON",
        "-DGP32EMU_CPU_PROFILE=ON", "-DGP32EMU_ENABLE_IPO=OFF"]
    environment = dict(os.environ)
    if args.kind == "aarch64":
        configure += ["-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
                      "-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-aarch64;-L;/usr/aarch64-linux-gnu"]
    if args.kind == "ubsan":
        sanitizer = "-fsanitize=undefined -fno-sanitize-recover=undefined"
        configure += ["-DCMAKE_C_FLAGS=" + sanitizer, "-DCMAKE_EXE_LINKER_FLAGS=" + sanitizer,
                      "-DCMAKE_SHARED_LINKER_FLAGS=" + sanitizer]
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    metadata = {"kind": args.kind, "source_head": args.frozen_head, "source_tree": git("rev-parse", "HEAD^{tree}"),
        "source_directory": str(source), "source_sha256": hashes, "fresh_build": True,
        "compiler": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
        "compiler_target": subprocess.check_output([compiler, "-dumpmachine"], text=True).strip(),
        "host": dict(platform.uname()._asdict()), "performance_measurement": False,
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "UBSAN_OPTIONS": environment.get("UBSAN_OPTIONS"), "stages": []}

    def save():
        (evidence / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    stages = [("configure", configure),
              ("inventory", [ctest, "--test-dir", str(build), "--show-only=json-v1"]),
              ("build", [cmake, "--build", str(build), "-j", str(args.jobs)]),
              ("ctest", [ctest, "--test-dir", str(build), "--output-on-failure", "-j", str(args.jobs)])]
    code = 0
    for name, command in stages:
        row = {"name": name, "command": command,
               "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
        metadata["stages"].append(row)
        save()
        start = time.monotonic()
        with (evidence / (name + ".log")).open("w") as log:
            code = subprocess.call(command, cwd=source, env=environment, stdout=log, stderr=subprocess.STDOUT)
        row.update(returncode=code, elapsed_seconds=round(time.monotonic() - start, 3))
        print(args.kind, name, "exit=" + str(code), flush=True)
        if name == "inventory" and not code:
            inventory = json.loads((evidence / "inventory.log").read_text())
            metadata["test_names"] = [test["name"] for test in inventory["tests"]]
            metadata["test_count"] = len(metadata["test_names"])
            if metadata["test_count"] != args.expected_tests:
                code = 3
                print("Unexpected test count:", metadata["test_count"], flush=True)
        save()
        if code:
            print((evidence / (name + ".log")).read_text()[-9000:], flush=True)
            break
    for name in ["LastTest.log", "LastTestsFailed.log"]:
        old = build / "Testing/Temporary" / name
        if old.exists():
            shutil.copyfile(old, evidence / name)
    commands_file = build / "compile_commands.json"
    if commands_file.exists():
        shutil.copyfile(commands_file, evidence / "compile_commands.json")
        commands = json.loads(commands_file.read_text())
        metadata["compile_command_count"] = len(commands)
        metadata["all_compile_commands_c23"] = all(
            "-std=c2x" in command["command"] or "-std=c23" in command["command"] for command in commands)
        if not metadata["all_compile_commands_c23"]:
            code = 4
        if args.kind == "ubsan":
            metadata["all_compile_commands_ubsan"] = all("-fsanitize=undefined" in command["command"] and
                "-fno-sanitize-recover=undefined" in command["command"] for command in commands)
            if not metadata["all_compile_commands_ubsan"]:
                code = 4
    metadata["source_unchanged_after_test"] = all(
        hashlib.sha256((source / name).read_bytes()).hexdigest() == value for name, value in hashes.items())
    if not metadata["source_unchanged_after_test"]:
        code = 4
    metadata.update(returncode=code, finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    metadata["log_sha256"] = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in evidence.glob("*.log")}
    save()
    raise SystemExit(code)


if __name__ == "__main__":
    main()
