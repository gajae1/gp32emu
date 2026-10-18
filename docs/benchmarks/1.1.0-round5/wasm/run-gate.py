#!/usr/bin/env python3
"""Record the final round-five freestanding WASM gate without changing the source tree."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    source, output, evidence = [p.resolve() for p in (args.source, args.output, args.evidence)]
    expected = "865f88e2d095f71ac80935bd775a38f3b8985246"
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if head != expected:
        raise RuntimeError(f"source must be frozen at {expected}, got {head}")
    if output.exists():
        raise RuntimeError(f"fresh output directory required: {output}")
    output.mkdir(parents=True)
    evidence.mkdir(parents=True, exist_ok=True)
    tracked = subprocess.check_output([
        "git", "ls-files", "-z", "Makefile.wasm", "cmake/c_standard.mk",
        "cmake/c_standard_probe.c", "src", "include", "wasm", "web",
        "tests/wasm_runtime_test.c", "tests/wasm_runtime_test.mjs"], cwd=source)
    paths = [Path(p.decode()) for p in tracked.split(b"\0") if p]
    before = {str(p): sha(source / p) for p in paths if (source / p).is_file()}
    command = [
        "make", "-f", "Makefile.wasm", "build", "test", "GP32EMU_C_STD=c23",
        "GP32EMU_REQUIRE_C23=1", "WASM_CC=clang-18", "WASM_LD=wasm-ld-18",
        "NODE=node", f"BUILD_DIR={output / 'build'}", f"OUT_DIR={output / 'web'}", "-j2"]
    manifest = {
        "source_commit": head,
        "source_tree": subprocess.check_output(["git", "rev-parse", "HEAD^{tree}"], cwd=source, text=True).strip(),
        "source_path": str(source), "output_path": str(output),
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "source_status_before": subprocess.check_output(["git", "status", "--short"], cwd=source, text=True),
        "source_sha256": before,
        "version_commands": [["clang-18", "--version"], ["wasm-ld-18", "--version"],
                             ["node", "--version"], ["make", "--version"]],
        "build_and_runtime_command": command,
        "working_directory": str(source),
        "notes": ["Functional gate only; no wall-clock performance claims.",
                  "Portable bytecode build; no native JIT is emitted for wasm32.",
                  "Production and test modules use the actual freestanding WASM libc.",
                  "The original runtime runner is unchanged and performs 32 saves, not loads."]}
    (evidence / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    with (evidence / "versions.log").open("w") as log:
        for version in manifest["version_commands"]:
            log.write("command: " + json.dumps(version) + "\n")
            log.flush()
            subprocess.run(version, cwd=source, check=True, stdout=log, stderr=subprocess.STDOUT)
    with (evidence / "build-runtime.log").open("w") as log:
        log.write("cwd: " + str(source) + "\ncommand: " + json.dumps(command) + "\n")
        log.flush()
        result = subprocess.run(command, cwd=source, stdout=log, stderr=subprocess.STDOUT)
    manifest["build_and_runtime_exit_code"] = result.returncode
    manifest["finished_utc"] = datetime.now(timezone.utc).isoformat()
    after = {str(p): sha(source / p) for p in paths if (source / p).is_file()}
    manifest["source_changes_during_gate"] = {str(p): [before.get(str(p)), after.get(str(p))]
        for p in paths if before.get(str(p)) != after.get(str(p))}
    manifest["modules"] = {str(p.relative_to(output)): {"bytes": p.stat().st_size, "sha256": sha(p)}
        for p in [output / "web/gp32_wasm_core.wasm", output / "build/wasm_runtime_test.wasm"] if p.exists()}
    manifest["evidence_sha256"] = {name: sha(evidence / name) for name in ["versions.log", "build-runtime.log"]}
    (evidence / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({key: manifest[key] for key in ["source_commit", "build_and_runtime_exit_code",
          "source_changes_during_gate", "modules"]}, indent=2))
    if result.returncode:
        raise SystemExit(result.returncode)
    if manifest["source_changes_during_gate"]:
        raise RuntimeError("frozen source changed while building")


if __name__ == "__main__":
    main()
