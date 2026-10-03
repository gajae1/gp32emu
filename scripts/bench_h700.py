"""Interleaved ABBA H700 benchmark with exactness and observed-clock evidence.

Requires Paramiko. Supply GP32_SSH_PASSWORD in the environment; the runner
stages only a standalone candidate under a dedicated remote directory
(default ROOT/bench-bin on the SD card, so repeated dev uploads cannot
fill the small tmpfs /tmp) and never replaces the baseline or a UI core.
Staged files are content-addressed and left in place for reuse.

Each row persists the exact remote command, the executable path and sha256,
the observed scaling_cur_freq/governor samples, the stdout log lines that
preceded the JSON result, and CPU/video/audio exactness against the first run.
Runs follow the ABBA order baseline,candidate,candidate,baseline; the output
file stays a top-level JSON list of rows and every row carries the same
"comparison" object. perf_success is true only when the sequence is complete,
every run is hash-exact, no clock monitor reported an error, every run has at
least two measured frequency samples, and every measured sample across all
runs reads the same host frequency, so a clock difference is never reported
as a speed gain.

Use --prime to run one unscored baseline invocation before ABBA when the
device's normal governor needs more time to reach a steady frequency. This
does not change the governor or relax the measured-clock qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import statistics
import threading
import time
import uuid

import paramiko

EXACT_KEYS = ("cycles", "pc", "cpsr", "clock", "audio_frames", "video_hash", "audio_hash")
RESULT_KEYS = EXACT_KEYS + ("fps", "elapsed")
EXPECTED_ORDER = ("baseline", "candidate", "candidate", "baseline")
MIN_CLOCK_SAMPLES = 2
LOG_TAIL_LIMIT = 1024   # kept stdout log prefix / stderr tail per row
# Any of these running holds the framebuffer/CPU and skews the benchmark.
RA_PROCESSES = ("ra64.h700", "ra64.gp32.h700", "ra32.h700",
                "ra64.universal", "ra32.universal", "retroarch")


def parse_result(text):
    """Return (result, log_prefix) from stdout, newest line first.

    gp32_bench prints exactly one line of JSON; core logs may precede it.
    Scanning lines in reverse and requiring the benchmark keys keeps a log
    line that happens to be JSON from being mistaken for the result.
    """
    lines = text.splitlines()
    for index in range(len(lines) - 1, -1, -1):
        line = lines[index].strip()
        if not line:
            continue
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict) and all(key in value for key in RESULT_KEYS):
            return value, "\n".join(lines[:index]).strip()
    raise ValueError("no benchmark JSON line found in stdout")


def clock_evidence(samples):
    """Observed host frequency for one measured window.

    The H700 steps its clock discretely, so steady means at least
    MIN_CLOCK_SAMPLES samples that all read the same kHz.
    """
    khz = sorted(sample["khz"] for sample in samples if "khz" in sample)
    evidence = {"sample_count": len(khz),
                "governors": sorted({sample.get("governor") for sample in samples
                                     if sample.get("governor")}),
                "steady": False}
    if not khz:
        evidence["note"] = "no frequency samples in the measured window"
        return evidence
    evidence.update({"min_khz": khz[0], "median_khz": statistics.median(khz),
                     "max_khz": khz[-1],
                     "steady": len(khz) >= MIN_CLOCK_SAMPLES and khz[0] == khz[-1]})
    return evidence


def build_comparison(rows, invocation):
    """Qualify the ABBA result; perf_success needs an exact, clock-matched run set."""
    order = [row["label"] for row in rows]
    observed = [sample["khz"] for row in rows for sample in row["measured_clock_samples"]
                if "khz" in sample]
    monitor_errors = [error for row in rows for error in row["monitor_errors"]]
    scores = {}
    for label in ("baseline", "candidate"):
        runs = [row for row in rows if row["label"] == label]
        fps = [row["result"]["fps"] for row in runs]
        scores[label] = {"runs": len(runs),
                         "executables": sorted({row["executable"] for row in runs}),
                         "sha256": sorted({row["sha256"] for row in runs}),
                         "fps": fps,
                         "median_fps": statistics.median(fps) if fps else None}
    minimum = min((row["clock_qualification"]["sample_count"] for row in rows), default=0)
    comparison = {"invocation": invocation, "exact_keys": list(EXACT_KEYS), "order": order,
                  "sequence_complete": order == list(EXPECTED_ORDER),
                  "cpu_video_audio_exact": bool(rows) and all(row["exact"] for row in rows),
                  "monitor_errors": monitor_errors,
                  "clock": {"matched": False, "observed_khz": sorted(set(observed)),
                            "measured_samples": len(observed),
                            "min_samples_per_run": minimum},
                  "roles": scores, "perf_success": False, "disqualifiers": []}
    disqualifiers = comparison["disqualifiers"]
    if not comparison["sequence_complete"]:
        disqualifiers.append("expected order %s; got %s"
                             % (",".join(EXPECTED_ORDER), ",".join(order) or "none"))
    if not comparison["cpu_video_audio_exact"]:
        disqualifiers.append("CPU/video/audio hash mismatch; no performance claim")
    if monitor_errors:
        disqualifiers.append("clock monitor errors: %d" % len(monitor_errors))
    if minimum < MIN_CLOCK_SAMPLES:
        disqualifiers.append("fewer than %d measured frequency samples in a run"
                             % MIN_CLOCK_SAMPLES)
    if comparison["clock"]["observed_khz"] and len(comparison["clock"]["observed_khz"]) > 1:
        disqualifiers.append("observed frequencies differ across runs: %s"
                             % ",".join(str(value)
                                        for value in comparison["clock"]["observed_khz"][:8]))
    comparison["clock"]["matched"] = bool(minimum >= MIN_CLOCK_SAMPLES and
                                          len(comparison["clock"]["observed_khz"]) == 1)
    if scores["baseline"]["median_fps"] and scores["candidate"]["median_fps"]:
        comparison["fps_ratio_candidate_over_baseline"] = round(
            scores["candidate"]["median_fps"] / scores["baseline"]["median_fps"], 4)
    comparison["perf_success"] = bool(comparison["cpu_video_audio_exact"] and
                                      comparison["sequence_complete"] and
                                      not monitor_errors and comparison["clock"]["matched"] and
                                      scores["baseline"]["runs"] and scores["candidate"]["runs"])
    if not comparison["perf_success"] and not disqualifiers:
        disqualifiers.append("comparison incomplete")
    return comparison


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.204")
    parser.add_argument("--user", default="spruce")
    parser.add_argument("--root", default="/mnt/SDCARD/gp32-dev")
    parser.add_argument("--remote-dir",
                        help="Absolute remote directory used to stage the candidate "
                             "binary (default ROOT/bench-bin); staged files are kept")
    parser.add_argument("--baseline", required=True, help="Existing remote executable")
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--args", required=True, help="Identical benchmark arguments for both executables")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--prime", action="store_true",
                        help="Run one unscored baseline invocation before ABBA")
    options = parser.parse_args()
    password = os.environ.get("GP32_SSH_PASSWORD")
    if password is None:
        parser.error("Set GP32_SSH_PASSWORD in the environment")
    remote_dir = (options.remote_dir or
                  options.root.rstrip("/") + "/bench-bin").rstrip("/")
    if (not remote_dir or not PurePosixPath(remote_dir).is_absolute()
            or ".." in PurePosixPath(remote_dir).parts):
        parser.error("--remote-dir must be an absolute path without '..' segments")
    digest = hashlib.sha256(options.candidate.read_bytes()).hexdigest()
    script_digest = hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest()
    remote = remote_dir + "/gp32-bench-" + digest[:16]
    arguments = " ".join(shlex.quote(value) for value in shlex.split(options.args))
    executables = {"baseline": options.baseline, "candidate": remote}
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    rows, invocation, comparison = [], None, None

    def execute(value, timeout=300):
        _, stdout, stderr = client.exec_command(value, timeout=timeout)
        out, err = stdout.read().decode(), stderr.read().decode()
        return stdout.channel.recv_exit_status(), out, err

    def command(value, timeout=300):
        status, out, err = execute(value, timeout)
        if status:
            raise RuntimeError(f"Remote command failed ({status}): {err.strip()}")
        return out.strip()

    def snapshot():
        lines = command("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq; "
                        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true; "
                        "cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || true", 10).splitlines()
        return {"khz": int(lines[0]),
                "governor": lines[1] if len(lines) > 1 else None,
                "temp_raw": lines[2] if len(lines) > 2 else None}

    try:
        client.connect(options.host, username=options.user, password=password,
                       timeout=8, look_for_keys=False, allow_agent=False)
        running = command("pidof " + " ".join(RA_PROCESSES) + " || true", 10)
        if running:
            raise RuntimeError("RetroArch is running (pids %s); benchmark deferred"
                               % running.replace("\n", " "))
        command("test -x " + shlex.quote(options.baseline), 10)
        baseline_digest = command("sha256sum " + shlex.quote(options.baseline), 10).split()[0]
        invocation = {"host": options.host, "user": options.user, "root": options.root,
                      "remote_dir": remote_dir,
                      "args": options.args, "args_argv": shlex.split(options.args),
                      "order": list(EXPECTED_ORDER),
                      "baseline": {"path": options.baseline, "sha256": baseline_digest},
                      "candidate": {"local_path": str(options.candidate),
                                    "remote_path": remote, "sha256": digest},
                      "bench_h700_sha256": script_digest}
        command("mkdir -p " + shlex.quote(remote_dir), 10)
        existing = command("sha256sum " + shlex.quote(remote) + " 2>/dev/null || true", 10)
        if existing and existing.split()[0] != digest:
            raise RuntimeError("Existing content-addressed executable has a different hash; refusing to replace it")
        invocation["candidate"]["reused"] = existing.split()[:1] == [digest]
        if existing.split()[:1] != [digest]:
            # Upload to a sibling temp first so the content-addressed path
            # only ever appears complete; staged files are never deleted.
            temp = remote + ".upload-" + uuid.uuid4().hex[:12]
            with client.open_sftp() as transfer:
                transfer.put(str(options.candidate), temp)
            if command("sha256sum " + shlex.quote(temp), 10).split()[0] != digest:
                raise RuntimeError("Uploaded executable hash mismatch")
            command("chmod 755 " + shlex.quote(temp), 10)
            command("mv -f " + shlex.quote(temp) + " " + shlex.quote(remote), 10)
        if command("sha256sum " + shlex.quote(remote), 10).split()[0] != digest:
            raise RuntimeError("Staged executable hash mismatch")
        command("test -x " + shlex.quote(remote), 10)
        if options.prime:
            prime_command = ("cd " + shlex.quote(options.root) + " && " +
                             shlex.quote(options.baseline) + " " + arguments)
            status, out, err = execute(prime_command)
            if status:
                raise RuntimeError(f"Priming run failed ({status}): {err.strip()}")
            prime_result, _ = parse_result(out)
            invocation["priming"] = {"command": prime_command, "result": prime_result,
                                     "after": snapshot(), "scored": False}
            print(json.dumps({"priming": invocation["priming"]}), flush=True)
        for label in EXPECTED_ORDER:
            executable = executables[label]
            samples, errors = [], []
            stop = threading.Event()

            def monitor():
                while not stop.is_set():
                    try:
                        samples.append({"at": time.monotonic(), **snapshot()})
                    except Exception as error:
                        errors.append(str(error))
                        return
                    stop.wait(1)

            worker = threading.Thread(target=monitor, daemon=True)
            run_command = ("cd " + shlex.quote(options.root) + " && " +
                           shlex.quote(executable) + " " + arguments)
            before = snapshot()
            worker.start()
            try:
                status, out, err = execute(run_command)
                finished_at = time.monotonic()
            finally:
                stop.set()
                worker.join(timeout=12)
            if status:
                raise RuntimeError(f"Benchmark run failed ({status}): {err.strip()}")
            try:
                result, log_prefix = parse_result(out)
            except ValueError as error:
                raise RuntimeError(f"Benchmark stdout had no JSON result: {error}; "
                                   f"tail={out.strip()[-300:]!r}")
            after = snapshot()
            reference = rows[0]["result"] if rows else result
            # This window is estimated from local receipt of the result. SSH
            # transit and process teardown can shift it; retain all samples
            # so a frequency transition cannot be hidden by this estimate.
            measured_samples = [sample for sample in samples
                                if finished_at - result["elapsed"] <= sample["at"] <= finished_at]
            mismatch = {key: [reference[key], result[key]] for key in EXACT_KEYS
                        if reference[key] != result[key]}
            expected_digest = digest if label == "candidate" else baseline_digest
            actual_digest = command("sha256sum " + shlex.quote(executable), 10).split()[0]
            if actual_digest != expected_digest:
                raise RuntimeError("Benchmark executable changed during comparison")
            row = {"label": label, "executable": executable, "sha256": expected_digest,
                   "command": run_command, "before": before, "after": after,
                   "samples": samples, "measured_clock_samples": measured_samples,
                   "clock_window_estimated": True, "monitor_errors": errors,
                   "clock_qualification": clock_evidence(measured_samples),
                   "stdout_log_prefix": log_prefix[-LOG_TAIL_LIMIT:],
                   "stderr_tail": err.strip()[-LOG_TAIL_LIMIT:],
                   "exact": not mismatch, "mismatch": mismatch, "result": result}
            rows.append(row)
            print(json.dumps({key: value for key, value in row.items()
                              if key not in ("samples", "measured_clock_samples")}), flush=True)
            if mismatch:
                raise RuntimeError("CPU/video/PCM mismatch; remaining runs cancelled")
    finally:
        client.close()
        if rows and invocation:
            comparison = build_comparison(rows, invocation)
            for row in rows:
                row["comparison"] = comparison
        options.output.parent.mkdir(parents=True, exist_ok=True)
        options.output.write_text(json.dumps(rows, indent=2), encoding="utf-8")
        if comparison is not None:
            print(json.dumps({"comparison": comparison}), flush=True)


if __name__ == "__main__":
    main()
