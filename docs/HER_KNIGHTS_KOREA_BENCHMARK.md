# Her Knights: Korean combat benchmark (2026-10-03)

## Current frontend audio evidence (2026-10-04, resume40)

A bounded 2,400-frame scripted combat replay used the corrected handheld
frontend plus the explicit FIFO-padding diagnostic from resume38. The current
core's final PC/cycles and last-1,200-frame source count match the standalone
replay: `0x0c00a6a0`, 6,441,235,000 cycles and 220,631 stereo source frames.
No replay frame had an empty source-audio buffer. The frontend accepted all
1,764,180 offered resampled stereo frames (including its initial BIOS run),
without partial or zero returns. All 2,590 ALSA writes returned a full period,
with zero errors or recoveries. The padding trace allocated successfully and
did not overflow.

Explicit FIFO padding is zero in output frames [96,000,1,872,000), a central
37-second window at 48 kHz. Padding occurs at the session edges; small counts
of zero-valued samples inside the window are not software-inserted padding.
This distinguishes that mechanism from silent game/resampler samples; it
does not prove clipping-free sound or physical speaker quality.

The late 1,200 frames sampled the host at 1,512 MHz and averaged 6.991 ms of
core execution. Frame-start intervals averaged 16.618 ms, but p99 was 31.935 ms
and maximum 32.131 ms, with 34 intervals over 20 ms. Video/audio callback
waiting contributes to long runs. CPU headroom and uninterrupted sample
delivery therefore do not establish smooth presentation or low input latency.

Separately, unsigned 8-bit PCM conversion no longer left-shifts negative
signed values in the SoC append and HLE PCM/SEF paths. A C23 shift-sanitized
fixture traps before the correction and passes afterwards. This is a
portability fix, not the identified cause of this BIOS/IIS playback's pacing.
The optimized handheld core and benchmark remain byte-identical to resume39, so
the installed production core was not needlessly replaced.

Evidence: `F:/GP32/results/resume40-audio/{before.json,after.json,
pcm-h700.json,her-equivalence.json,padding.csv,her-runtime-summary.json}` and
`F:/GP32/results/resume40-her-audio-runtime-verified.json`. The gameplay
screenshot is `resume40-her-audio-runtime.png`. The run exited normally and
returned to MainUI; installed binaries and checked settings retained their
hashes. The older measurements below describe their named revisions/scenes.

## Content and workload

`Her Knights - All for Princess - Deadline (Korea).smc` is **그녀의 기사단 강행돌파**.
[MAME's GP32 software catalog](https://github.com/mamedev/mame/blob/master/hash/gp32.xml)
maps this filename to `herknite`, the Korean title, and the exact dump verified here:
17,302,528 bytes; SHA-1 `4ced58d61f831cbfc0d0a369e559ad364cb71541`;
SHA-256 `b4c14265f79910d4cbc596c7b0cebbfd3ecffda07678ffe5b3e36fe221e4547d`.
Earlier Her Knights measurements used the separate European dump and boot/menu
windows. They are not interchangeable with this result.

The local EU 1.6.6 BIOS boots the Korean game. Automatic A pulses alone stay at
the title. Start, character/mode confirmation, and dialogue advances reach the
first palace combat scene with Lynnerd. Captures confirm enemies and combat at
the start and at input-script frames 1500 and 2400. This is one early battle,
not a worst-case boss, all characters, or a whole-game minimum.

Reproduction assets are outside the repository under `F:/GP32/results/`:

- `resume9-her9000.state`: combat entry state, generated locally from the original
  read-only ROM; no installed game save was used or modified.
- `resume9-her-combat.txt`: hold right for the first 180 frames and pulse A for
  15 out of each 30 frames; subsequent A pulses continue while standing.
- `resume9-her{9000,-fight1500,-fight2400}.png`: inspected scene captures.
- `resume9-scene.c`: external headless helper with state-save support.

The handheld developer copies live in `/mnt/SDCARD/gp32-dev/resume9-her/`.
Benchmark arguments, from `/mnt/SDCARD/gp32-dev`:

```text
--bios gp32166m.bin --smc resume9-her/her-korea.smc
--state resume9-her/combat.state --input-script resume9-her/combat.txt
--warmup 1200 --frames 1200 --jit
```

## Core throughput on the device

The integrated resume12 optimization batch improves the same battle workload
from **90.098 to 101.2165 core fps (+12.34%)**, in one alternating ABBA comparison.
The two baseline runs are 90.062/90.134 and candidate runs 101.392/101.041.
All seven CPU/video/PCM fields match and all 47 measured-window clock samples
are 1.512 GHz. This measures simulation throughput; it does not make the game's
own animation run at 101 fps. Evidence: `resume12-her-final-abba.json`.
Candidate benchmark SHA-256:
`8638208da39fdb292ced0da83e4cfa33c8e1330da326f492a87f24abb56c1b51`.

The integrated core also completed an 1800-frame normal handheld/RetroArch idle
battle run with ALSA PLAYBACK initialized and exit code 0. The mid-run capture
showed 60.27 fps; shutdown reported 1782 video frames pushed and 19 dropped.
No physical listening or ALSA interposer was used in this final run. Evidence:
`resume12-final-ra.log`, `resume12-final-ra.png`, `resume12-core-installed.json`.

The earlier lazy-validation-only comparison is retained below:

| Build | Two measured runs (fps) | Median (fps) |
| --- | --- | --- |
| Before lazy JIT cache validation | 81.828, 82.144 | 81.986 |
| Current lazy JIT cache validation | 90.680, 90.393 | 90.5365 |

The improvement is **10.43%** in an ABBA sequence. All 54 measured-window CPU
frequency observations were 1.512 GHz; conservative governor was unchanged.
CPU cycles, PC, CPSR, emulation clock, PCM frame count, video hash and PCM hash
match in all four runs. This establishes equivalence between these two AArch64
builds in this window; it does not establish hardware-accurate timing or general
interpreter/JIT equivalence.

Baseline benchmark SHA-256:
`801b88c8c0ba94194db64098fc291d1c6d915dcb79deb3a959079a3b66781665`.
Candidate benchmark SHA-256:
`491015a7631f892c4b3c70b0f669cc7c45c9576ab9fd1d40239e333885cbf4d4`.
Raw evidence: `F:/GP32/results/resume9-her-korea-combat-abba.json`.

Here fps means logical 60-Hz emulation steps processed per wall-clock second.
It is neither the game's animation rate nor RetroArch's presented frame rate.

## Core audio continuity

An external, separately instrumented harness observes the same 1200 measured
frames (20 emulated seconds). Its seven correctness fields match the stock
candidate benchmark. CSV row count and sample sums were independently checked.

| Observation | Result |
| --- | --- |
| Frames with PCM / empty / acquisition errors | 1200 / 0 / 0 |
| Stereo PCM frames | 220,631 |
| Samples per emulation frame | 183–184 |
| Reported source rate / rate changes | 11,035 Hz / 0 |
| PCM duration at the reported rate | 19.993747 s |
| Different framebuffer observations | 305 / 1200 (15.25 per emulated second) |
| Core frame work p50 / p95 / p99 / max | 11.625 / 15.752 / 15.952 / 16.499 ms |

Each 60-frame window supplied approximately one second of PCM. The small source
duration/rate discrepancy remains observable; these counters do not establish
long-term clock synchronization. A changed framebuffer is a sampled byte
comparison, not a measurement of the original hardware's renderer or refresh
rate. The existing fixed-60-Hz and CPU clock approximations still apply.

The instrumented process ran at 80.428 logical fps. Its framebuffer comparison
and statistics add overhead and are outside the per-frame `wall_ms` samples;
use the stock ABBA result for the speedup. These are core timings, without the
frontend driver work. Absence of empty PCM does not rule out clipping or audible
artifacts elsewhere in the pipeline.

Evidence: `resume9-her-audio-h700.{json,csv}` and
`resume9-verify-evidence.py` under `F:/GP32/results/`.
Harness: `resume9-her-audio-bench.c`; handheld executable SHA-256
`02e066452df2a0f3a635a8de4935480732513c76d8644b6039d261cfbb6c5b5d`.

### Cross-host replay follow-up

The script contains two frame-zero SETs. A subsequent investigation found that
the original input parser did not preserve the ordering of equal frame/action
events: Windows initially applied RIGHT while handheld applied RIGHT+A. The parser
now uses insertion order to break that tie. The original script then produces
identical CPU/video/PCM results on both hosts over the 1200/1200 run, matching
the handheld hashes above. Thus these handheld measurements remain valid, while older
Windows runs of the duplicate-event script must not be treated as identical
input before that fix. Evidence: F:/GP32/results/resume10-input-cross-host-exact.json.

## Actual RetroArch attempt and remaining limit

### Normal device launch follow-up (resume11)

A temporary `GP32 Audio Bench` app now launches through the device's normal menu
exit/respawn path. This releases MainUI's PCM device before RetroArch starts.
The separate candidate core and developer configuration above were used; the
installed core, platform configuration, common RetroArch configuration and
conservative governor match their prior hashes/values after the run.

ALSA initialized successfully as S16_LE with four 768-frame periods and a
3072-frame buffer. The 1800-frame idle-combat run exited normally; launch through
shutdown took 33 seconds and RetroArch reported 29 seconds of content runtime.
A mid-run screenshot shows 59.98 fps, and the final OSD shows 60.29 fps. Threaded
video logged 1784 frames pushed and 17 dropped. This is a presentation measurement
for this scene, not the animation rate or a whole-game minimum.

The monitor captured 29 RUNNING PCM samples owned by RetroArch, all with one
unchanged trigger timestamp; queued delay was 2336–3072 PCM frames. No sampled
status was XRUN. Sampling at roughly one-second intervals cannot exclude short
recoveries. The OSD displayed audio underrun 4.22% and average saturation 59.59%.
In [RetroArch v1.22.2](https://github.com/libretro/RetroArch/blob/v1.22.2/audio/audio_driver.c),
the underrun statistic counts samples with at least 75% free buffer space.
For alsathread that is the software FIFO, not the ALSA hardware ring. It is not
an audible-dropout percentage. The
[worker](https://github.com/libretro/RetroArch/blob/v1.22.2/audio/drivers/alsathread.c)
can pad a FIFO shortfall with silence or recover an ALSA underrun without a
successful-recovery log. Direct counters are needed to distinguish these cases.
There is no physical speaker recording or listening acceptance yet. The menu
returned normally after the automatic stop.

Evidence under `F:/GP32/results/`: `resume11-normal.log`,
`resume11-normal.exit`, `resume11-normal.png`, `resume11-ui-during.png`,
`resume11-normal-monitor.json`, `resume11-normal-summary.json`,
`resume11-ui-returned.png`, and `resume11-device-after.json`.
The temporary app is `/mnt/SDCARD/App/GP32AudioBench`; its source bundle is
`F:/GP32/results/resume11-GP32-Audio-Benchmark/`. It does not replace the installed
GP32 launcher/core or modify audio latency, volume, or CPU governor.

### Interposed ALSA counters (resume11 final evidence)

Two interposed probe libraries counted every libasound call of the developer
runs (resume11-timeline-alsa.json, resume11-combat-alsa.json, summarised in
resume11-final-evidence.json). Both resolved
/usr/lib/aarch64-linux-gnu/libasound.so.2 with an empty problem field.

| Counter | timeline run | combat run |
| --- | --- | --- |
| writei calls | 1,985 | 1,982 |
| written frames | 1,524,480 | 1,522,176 |
| wait / recover / prepare / start calls | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| EPIPE / ESTRPIPE / EAGAIN / other errors | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| recovery calls (ok / fail) | 0 / 0 | 0 / 0 |
| all-zero stereo frames | 86,352 | 84,689 |
| trailing zero frames / calls / longest run | 86,019 / 114 / 768 | 84,371 / 110 / 768 |

No EPIPE or ESTRPIPE was observed, the driver never invoked the ALSA recovery
path, and the longest contiguous zero run equals exactly one ALSA period
(768 frames).

Zero-frame accounting is only a middle-silence heuristic. The
per-48,000-written-frames histogram of the timeline run puts almost all zeros in
the first bucket (37,633) and the last two buckets (11,909 and 36,480), while
the 29 steady middle seconds contain 330 zero stereo frames in total, 5-21 per
second. An all-zero frame counter cannot separate the game's own naturally
silent PCM from a region lost downstream, so those small middle counts are not
evidence of a dropout - and their smallness is not proof of audible continuity
either.

Scope limits: this run used the auto-loaded combat-entry state without the
scripted attack input, so it is an idle battle window, not a proven active
attack workload. There is no physical speaker recording or listening, so
audible output quality remains unaccepted. Installed core, platform
configuration, common RetroArch configuration and governor were read back
unchanged (installed_unchanged all true in resume11-final-evidence.json).

Probe artifacts: gp32_alsa_probe.c SHA-256
e06365910d7baedae57dc422f6fed9c85142126e83a00e93a4f4be4048c11d1f,
libgp32_alsa_probe.so
b7ea6b229b5e53e9fcb55be92fd5f4a650394fe30dcaf8e0560f2bc7e96bce7e,
libgp32_alsa_timeline.so
01a9fd1621a25b5cf1210d664356c0f9de380b27858d78cef6214131927b539e.

### Earlier direct-SSH attempt (resume9)

The candidate core ran 1800 frames through installed `ra64.h700` using a separate
developer configuration and save directories. The original GL/fbdev_mali,
threaded video, alsathread, 64-ms audio latency, and volume settings were retained.
The combat auto-state loaded and the final screenshot reported 60.06 fps.
Shutdown logged 1794 video frames pushed and 7 dropped. This was an idle combat
scene, not the scripted attack workload above.

**This run is not audio validation:** ALSA returned `Device or resource busy`,
and RetroArch continued without audio. `/dev/snd/pcmC0D0p` was owned by handheld
MainUI (process 10400, audio thread 10511). No menu process was killed/stopped.
An OSD underrun value of zero with a failed audio driver proves nothing.
Raw evidence: `resume9-her-ra-run.json`, `resume9-her-ra.log`, and
`resume9-her-ra-final.png` under `F:/GP32/results/`.

Read-only inspection of the installed PyUI source explains this contention:
normal game launch writes `cmd_to_run.sh`, deinitializes the display, and exits
PyUI before the outer launcher starts the game. The audio reinitialization flag
immediately reopens the device and is not a safe release interface. A direct
SSH-launched RetroArch alongside the menu therefore does not reproduce normal
launch ownership. Actual sound acceptance needs the normal UI launch path;
no process-exit injection or audio-reinitialization race was attempted. Source
excerpts and findings: `F:/GP32/results/resume9-ui-audio-investigation.md`.

During preparation, accumulated development binaries plus the new ROM/state
filled the device's 256-MiB `/tmp`. The new partial state was removed and this
turn's ROM/state assets moved to the SD development directory, freeing about
23 MiB. This does not establish the cause of the user's earlier OS incident.
Installed core, launcher configuration, common RetroArch configuration, governor,
and volume were not deliberately changed. Final `resume9-device-after.json`
readback confirms all three installed-file hashes and the governor match the
before snapshot, with no RetroArch process left running. No production emulator
code was changed for this benchmark.
