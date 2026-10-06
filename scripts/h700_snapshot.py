#!/usr/bin/env python3
"""Read-only H700 / SpruceOS snapshot for OS-corruption diagnosis.

Writes one local JSON file with the incident-relevant live state: kernel log
tail, /proc/meminfo, the live RetroArch pid/status/maps, the two known hashes
(GP32 core and Emu/GP32/config.json platform config), and CPU freq/governor/temp.

Every remote command is read-only: no sudo, no process is started or killed,
no remote file is created, edited or deleted. Missing files and permission
errors are recorded in "errors" and per capture instead of aborting the run.

Paths follow docs/PORTABILITY_PROGRESS.md: the
core is /mnt/SDCARD/Emu/GP32/gp32emu_libretro.so and the RA process names are
ra64.h700 and the GP32-only ra64.gp32.h700. Set GP32_SSH_PASSWORD in the
environment.
"""
import argparse
import datetime
import json
import os
import re
import shlex
import sys
from pathlib import Path

HOST = os.environ.get("GP32_H700_HOST") or None
USER = "spruce"
ROOT = "/mnt/SDCARD"
PROCESSES = ("ra64.h700", "ra64.gp32.h700")
KERNEL_LINES = 300
MAPS_LIMIT = 262144
TIMEOUT = 20


def utc_stamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%SZ")


def read_capped(stream, limit):
    """Drain one channel stream, keeping at most limit bytes."""
    chunks, kept, truncated = [], 0, False
    while True:
        data = stream.read(65536)
        if not data:
            break
        if kept < limit:
            chunk = data[: limit - kept]
            chunks.append(chunk)
            kept += len(chunk)
            truncated = truncated or len(chunk) < len(data)
        else:
            truncated = True
    return b"".join(chunks).decode("utf-8", "replace"), truncated


def run(client, command, limit=65536):
    """One read-only remote command; failures are data, not exceptions."""
    entry = {"command": command, "exit": None, "ok": False, "stdout": "", "stderr": ""}
    channel = None
    try:
        stdin, stdout, stderr = client.exec_command(command, timeout=TIMEOUT)
        stdin.close()
        channel = stdout.channel
        channel.settimeout(TIMEOUT)
        out, truncated = read_capped(stdout, limit)
        err, _ = read_capped(stderr, 8192)
        entry.update(exit=channel.recv_exit_status(), stdout=out, stderr=err, truncated=truncated)
        entry["ok"] = entry["exit"] == 0
    except Exception as error:
        entry["error"] = f"{type(error).__name__}: {error}"
    finally:
        if channel is not None:
            channel.close()
    return entry


def text_of(captures, label):
    return captures.get(label, {}).get("stdout", "")


def collect(client):
    """Run every read-only capture; returns captures, errors, RA pids, kernel ok."""
    captures, errors = {}, []

    def record(label, command, limit=65536):
        entry = run(client, command, limit)
        captures[label] = entry
        if not entry["ok"]:
            errors.append("%s: %s" % (label, entry.get("error") or entry["stderr"].strip()
                                      or "exit %s" % entry["exit"]))
        return entry

    kernel = record("kernel_dmesg", f"out=$(dmesg 2>&1); rc=$?; "
                                    f"printf '%s\\n' \"$out\" | tail -n {KERNEL_LINES}; exit $rc")
    if not kernel["ok"]:
        record("kernel_kern_log", f"tail -n {KERNEL_LINES} /var/log/kern.log")
    record("meminfo", "cat /proc/meminfo")

    pidof = record("ra_pidof", "pidof " + " ".join(PROCESSES), 1024)
    pids = pidof["stdout"].split() if pidof["ok"] else []
    for pid in pids:
        base = f"/proc/{pid}"
        record(f"ra{pid}_status", f"cat {base}/status")
        record(f"ra{pid}_maps", f"cat {base}/maps", MAPS_LIMIT)
        record(f"ra{pid}_exe", f"readlink {base}/exe", 4096)

    for label, path in (("core_sha256", f"{ROOT}/Emu/GP32/gp32emu_libretro.so"),
                        ("platform_config_sha256", f"{ROOT}/Emu/GP32/config.json"),
                        ("retroarch_config_sha256", f"{ROOT}/Saves/ra-configs/retroarch-AnbernicXX720480NoStick.cfg")):
        record(label, f"sha256sum {shlex.quote(path)}", 4096)

    record("cpu_governor", "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", 256)
    record("cpu_cur_freq", "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 256)
    record("soc_temp", "cat /sys/class/thermal/thermal_zone0/temp", 256)
    return captures, errors, pids, kernel["ok"]


def find(text, pattern):
    match = re.search(pattern, text, re.M)
    return match.group(1) if match else None


def as_int(value):
    return int(value) if value and value.lstrip("-").isdigit() else None


def write_document(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, ensure_ascii=False), encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(prog="h700_snapshot.py", description=__doc__,
                                     formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--host", default=HOST, required=HOST is None, help="SSH host of the H700 device (or set GP32_H700_HOST)")
    parser.add_argument("--user", default=USER, help="SSH user")
    parser.add_argument("--output", type=Path, default=None,
                        help="Local JSON path (default: h700-snapshot-<UTC>.json)")
    options = parser.parse_args(argv)

    password = os.environ.get("GP32_SSH_PASSWORD")
    if not password:
        parser.error("GP32_SSH_PASSWORD is not set (never stored or prompted)")
    try:
        import paramiko
    except ImportError as error:
        parser.error(f"Paramiko is required to connect: {error}")

    output = options.output or Path(f"h700-snapshot-{utc_stamp()}.json")
    document = {"schema": "h700-snapshot/1",
                "captured_at_utc": datetime.datetime.now(datetime.timezone.utc)
                                   .strftime("%Y-%m-%dT%H:%M:%SZ"),
                "target": {"host": options.host, "user": options.user, "root": ROOT},
                "summary": {}, "captures": {}, "errors": []}

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        client.connect(options.host, username=options.user, password=password,
                       timeout=TIMEOUT, banner_timeout=TIMEOUT, auth_timeout=TIMEOUT,
                       look_for_keys=False, allow_agent=False)
    except Exception as error:
        document["connection_error"] = f"{type(error).__name__}: {error}"
        try:
            write_document(output, document)
        except OSError as write_error:
            print(f"connection failed and {output} could not be written: {write_error}",
                  file=sys.stderr)
            return 1
        print(f"connection to {options.user}@{options.host} failed; wrote {output}",
              file=sys.stderr)
        return 2

    try:
        captures, errors, pids, kernel_ok = collect(client)
    finally:
        client.close()
    document["captures"], document["errors"] = captures, errors

    temp_raw = as_int(find(text_of(captures, "soc_temp"), r"^(-?\d+)"))
    meminfo = text_of(captures, "meminfo")
    retroarch = {}
    for pid in pids:
        status = text_of(captures, f"ra{pid}_status")
        retroarch[pid] = {"state": find(status, r"^State:\s+(.+)$"),
                          "rss_kb": as_int(find(status, r"^VmRSS:\s+(\d+) kB")),
                          "exe": text_of(captures, f"ra{pid}_exe").strip() or None,
                          "maps_kept_bytes": len(text_of(captures, f"ra{pid}_maps"))}
    document["summary"] = {
        "kernel_log_source": "dmesg" if kernel_ok else "kern.log" if captures.get(
            "kernel_kern_log", {}).get("ok") else None,
        "mem_total_kb": as_int(find(meminfo, r"^MemTotal:\s+(\d+) kB")),
        "mem_available_kb": as_int(find(meminfo, r"^MemAvailable:\s+(\d+) kB")),
        "retroarch_pids": pids,
        "retroarch": retroarch,
        "core_sha256": (text_of(captures, "core_sha256").split() or [None])[0],
        "platform_config_sha256": (text_of(captures, "platform_config_sha256").split()
                                   or [None])[0],
        "retroarch_config_sha256": (text_of(captures, "retroarch_config_sha256").split()
                                    or [None])[0],
        "governor": text_of(captures, "cpu_governor").strip() or None,
        "cur_freq_khz": as_int(text_of(captures, "cpu_cur_freq").strip()),
        "temp_raw": temp_raw,
        "temp_c": round(temp_raw / 1000.0, 1) if temp_raw is not None else None,
    }
    try:
        write_document(output, document)
    except OSError as error:
        print(f"could not write {output}: {error}", file=sys.stderr)
        return 1

    print(f"{output} ({output.stat().st_size} bytes, {len(captures)} captures, {len(errors)} errors)")
    print("kernel log: %s; retroarch: %s" % (
        document["summary"]["kernel_log_source"] or "unavailable",
        " ".join(pids) if pids else "not running"))
    for line in errors[:8]:
        print("  " + line, file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
