# RetroArch 1.22.2 ALSA wait correction

This is an optional **frontend source patch for OS/RetroArch builders**. It is
not part of the gp32emu core build and is not installed by the Spruce package.
The tested handheld retains its original system RetroArch binary and settings;
the optional GP32-only selection described below uses a separate frontend.

## Source and scope

`69a4f0e-alsa-fifo-wakeup.patch` backports the synchronization fix from
[RetroArch c55718842aa53425e9851acf4dc4f91844116cad](https://github.com/libretro/RetroArch/commit/c55718842aa53425e9851acf4dc4f91844116cad)
to `audio/drivers/alsathread.c` at
[69a4f0ea1e8aaf442ae4858f2e7f2b31a1776576](https://github.com/libretro/RetroArch/blob/69a4f0ea1e8aaf442ae4858f2e7f2b31a1776576/audio/drivers/alsathread.c).
The original file SHA-256 is
`b21c4405d1b1c73429095db274f948225f2d61cb6fff01e29411b67b58612688`.
Retain RetroArch's licensing and distribution requirements when rebuilding it.

The old blocking writer checks FIFO space under `fifo_lock`, releases that
mutex, then waits using another mutex. A worker can free space and signal in
between, before the writer becomes a waiter. Waiting with the FIFO mutex makes
the predicate check and waiter registration atomic with respect to that worker.
The patch follows the upstream shared-structure change for playback, capture
and shutdown; it does not import later buffering or driver rewrites.

From a separate checkout of that exact RetroArch revision using LF line endings,
with the patch path replaced by its absolute location:

```sh
git -c core.autocrlf=false -c core.eol=lf apply --check /path/to/69a4f0e-alsa-fifo-wakeup.patch
git -c core.autocrlf=false -c core.eol=lf apply /path/to/69a4f0e-alsa-fifo-wakeup.patch
```

On Windows, disable `core.autocrlf` when creating the checkout. Setting it only
on `git apply` does not convert files already checked out with CRLF. Verify the
LF source against the SHA-256 above before applying.

Use the OS project's existing build/package process after applying. This
repository does not supply a rebuilt frontend. Do not apply this legacy patch
to a newer driver which already incorporates the upstream fix.

## Focused reproduction

`lost_wakeup.c` is a small pthread FIFO model, not a replacement audio driver.
Barriers force the problematic interleaving without timing sleeps. A 100 ms
watchdog bounds the observation; real RetroArch uses an untimed wait here.

```sh
cc -std=c23 -O2 -Wall -Wextra -Werror -pthread lost_wakeup.c -o lost_wakeup
./lost_wakeup original       # expected exit 1: space exists, progress timed out
./lost_wakeup fixed          # expected exit 0: write completes
./lost_wakeup fixed-shutdown # expected exit 0: shutdown releases the waiter
```

Exit 77 in original mode means a permitted spurious wake rescued the execution;
it is inconclusive, not a successful reproduction. A compiler with only C11
support can build the same model with `-std=c11`.

The original failure and both corrected outcomes were verified with Windows
winpthreads and native H700 Linux pthreads. Patch application to the exact
upstream source also passed. Complete frontend builds and real ALSA playback
were subsequently checked as described below. This targeted fix does not
establish that every access to the driver's `volatile thread_dead` or concurrent
teardown is free of races.

## Device evidence and adoption limit

A 900-frame Tomak Korea replay observed four callbacks which waited twice for
FIFO space. One callback took 22.254 ms, including waits of 6.056 and 15.942 ms.
The device uses 768-frame ALSA periods at 48 kHz (16 ms), while one emulation
frame submits roughly 800 resampled frames. **Two waits alone do not prove a
lost notification:** the trace did not measure the unlock-to-wait window.

Submitting half the core audio before video and half afterwards preserved PCM
but left a roughly 30.55 ms interval p99, so that core change was rejected.
A separate temporary `audio_driver = "alsa"` run still had early long intervals
and one recovered ALSA underrun, so it was not adopted as a setting change.
Neither experiment altered the user's configuration files or installed core.

## Complete frontend comparison (2026-10-03)

Both comparison binaries used upstream 69a4f0e plus all four common patches from
[Spruce's historical H700 recipe at de0d03c](https://github.com/spruceUI/RA/blob/de0d03cbbe77936e1b590d68677858547b299e2f/build-h700.sh):
`0001-spruce-igm.patch`, `0002-portrait-panel-landscape-rotation.patch`,
`0003-sysfs-rumble-fallback.patch`, and `0004-sdl-gl-reapply-es-profile.patch`.
The feature selection follows that recipe, including Mali fbdev/EGL/GLES,
SDL2, ALSA, udev, FreeType, networking, SSL and built-in filters/zlib.

The diagnostic pair was cross-compiled on Windows with Zig 0.13.0 / Clang
18.1.6, targeting `aarch64-linux-gnu.2.17` and `cortex_a53`, using Git Bash and
GNU make. Ubuntu focal arm64 development headers and EGL/GLES link interfaces
were used with the device's runtime libraries. Private SDK adaptations included
system-header search precedence for bundled zlib, a FreeType include override,
LTO and removal of unsupported `rpath-link`. No SDK or replacement runtime
libraries were deployed. The compiler differs from the installed GCC 9.4.0
frontend; compare the two rebuilt binaries with each other, not as a
patch-only comparison against the installed binary.

Only `audio/drivers/alsathread.c` was recompiled between the baseline and
corrected frontend links. Both binaries passed native `--version` and real
Tomak Korea playback. Binary SHA-256:

- Baseline: `1c858af7a66d5b372dc6c6a75697e79f8080fa324541da65ee72614e5808ec57`
- Corrected: `7d2c80673d114f448b0e2c09addeff023387dc09efc1506927b5a4d940bfee85`

Execution order was baseline, corrected, corrected, baseline, using the same
900-frame replay, diagnostic core and configuration. All sampled CPU clocks
were 1,512 MHz. The table covers the last 600 frames, after 300 warm-up frames;
the initial state-load interval is excluded.

| Run | Core mean (ms) | Frontend interval p99 (ms) | Interval max (ms) | Intervals over 20 ms |
| --- | ---: | ---: | ---: | ---: |
| Baseline 1 | 8.032 | 29.944 | 32.010 | 17 |
| Corrected 1 | 8.049 | 17.270 | 17.520 | 0 |
| Corrected 2 | 8.062 | 17.288 | 18.143 | 0 |
| Baseline 2 | 8.014 | 30.289 | 31.959 | 11 |

Every run matched the standalone per-frame guest PC, source PCM count/nonzero
count and IIS configuration, accepted all 661,990 offered stereo frames, and
recorded no ALSA errors or recoveries. All four final screenshots were
pixel-identical to the installed frontend's reference. MainUI returned normally;
the production frontend, core and configuration hashes remained unchanged.

This supports reduced long frame intervals in this scene. It does not prove
that every earlier long interval was a lost notification, that all games have
stable pacing, or that speaker output and physical input latency are accepted.
Earlier unmodified runs sometimes also had approximately 17 ms interval p99.

The installed frontend matches the published Spruce v4.4.0 binary except for
61 bytes in eight read-only-data ranges containing Korean menu labels. These
localizations are not yet included in the rebuilt pair. Preserve them and
validate normal menu/input flows before adopting a rebuilt frontend globally.
The optional patch is still not installed by the gp32emu package.

Local evidence: `F:/GP32/results/resume34-frontend/build-manifest.json`,
`runtime-abba.json`, `published-comparison.json`, `build-fixed-2.log`, and
`F:/GP32/results/resume34-{base,fixed,fixed2,base2}-audio-runtime-verified.json`.

## Preserving the Korean in-game menu

`de0d03c-korean-igm.patch` translates the historical vendor `spruce_igm.c`
directly. Apply it after the four Spruce patches above; the ALSA correction is
independent. It preserves the installed menu's Korean labels and additionally
translates the automatic slot label. It does not alter RetroArch's general
translation tables, controller mappings or menu layout.

The vendor file's LF SHA-256 before this patch is
`0a5470818eb935fd9f61eb23182373a6f7d85087e8d247a382ee30958d42ecba`.
The patched file's SHA-256 is
`8a3111e08f7de86b32b438debd0a5f11c706c21ed0a4bbdbd285bd78991842fd`.
Use `git apply --check` and `git apply` with its absolute patch path, as for
the ALSA patch. The existing menu font must contain Korean glyphs.

A full build containing both patches has SHA-256
`e4a4ea8feebe9776b867a23f61380c7fb2b78605a4bcf6d8e31b6144d70e4b50`.
On the H700's 720x480 display, the title, all six menu labels and numbered slot
labels were visually checked without clipping. The existing network menu
hotkey opened and closed the menu; gameplay resumed for 714 further core runs.
All 863 replay frames matched the standalone reference prefix, and all 634,806
offered audio frames were accepted with no ALSA errors/recoveries. The fixed
presentation-frame cap includes paused frames, so this menu interaction run
is not a 900 emulated-frame performance comparison.

Evidence: `F:/GP32/results/resume35-frontend/{localization.json,build.log,
preflight.json,menu.png,menu-validation.json}`. This closes the missing-label
issue for the rebuilt in-game menu. The system-wide RetroArch remains unchanged.
GP32-only adoption is described below; physical button navigation and
automatic-slot rendering remain separate acceptance work.

## GP32-only selection in Spruce (2026-10-04)

`gp32-h700-frontend-selection.patch` updates Spruce's
`spruce/scripts/emu/lib/ra_functions.sh` after its usual 32/64-bit selection.
It selects `RetroArch/ra64.gp32.h700` only when all four conditions hold:

- The system is `GP32`.
- The selected core is `gp32emu`.
- The normal frontend would be `ra64.h700`.
- The separate frontend exists and is executable.

Other systems, other cores, 32-bit and generic 64-bit frontends keep their
existing selection. If the separate file is absent, the original H700 frontend
is used. No emulator JSON, core options or RetroArch settings need changing.

For OS builders, first build and validate the frontend with the historical
vendor patches, ALSA correction and desired menu localization described above.
With RetroArch stopped, retain a backup of `ra_functions.sh`, install the
validated executable as `RetroArch/ra64.gp32.h700`, and apply this selection
patch to a staging copy of the SD-card tree:

```sh
git apply --check /path/to/gp32-h700-frontend-selection.patch
git apply /path/to/gp32-h700-frontend-selection.patch
sh -n spruce/scripts/emu/lib/ra_functions.sh
```

Run those commands from the staging tree root. Stop if the patch does not match
the local Spruce version; do not replace an updated OS script wholesale.
Deploy the patched script only after checking the candidate's dependencies and
native `--version`. To revert selection, move the separate frontend out of that
filename while RetroArch is stopped, or restore the saved script. An OS update
may replace the script and require reapplying the small patch.

The tested device has this selector and the localized corrected binary
`e4a4ea8feebe9776b867a23f61380c7fb2b78605a4bcf6d8e31b6144d70e4b50` installed.
The original `ra64.h700` and GP32/RetroArch config hashes are unchanged. The
selector's seven routing cases, including a missing binary, passed on-device.
The installed `run_retroarch` function then launched the installed GP32 core
and new frontend, matched the existing ANBERNIC controller profile, displayed
Tomak gameplay, exited with code 0 and returned to MainUI without ALSA errors.
That run used private config/save paths and a bounded-duration wrapper. It
does not establish full `standard_launch.sh` bookkeeping or physical input/audio
acceptance. No new frame-pacing speedup is claimed from this integration run.

Spruce's unchanged CPU-pinning helper emitted `sched_setaffinity: Invalid
argument`: the launcher passes `23`, while the bundled `pin_cpu` expects a
comma-separated list such as `2,3`. The observed frontend still had CPUs 0-3
available. No governor or affinity policy was changed as part of frontend
selection; the follow-up experiment below evaluates this issue.

Local evidence: `F:/GP32/results/resume36-integration/{installed.json,
routing.txt,source-manifest.json}` and
`F:/GP32/results/resume36-launch2/{runtime-verified.json,frontend.exit,
active-exe.txt,active-status.txt,runtime.log,final.png}`.

## Preserve GP32's inherited CPU affinity

Apply `gp32-h700-affinity.patch` to the same staging tree when using the
GP32-only frontend. It changes `spruce/scripts/emu/lib/general_functions.sh`
so `pin_to_dedicated_cores` returns immediately for `ra64.gp32.h700`.
Other process names retain the OS's existing behavior. This removes the failed
pinning request while preserving the CPU set inherited from the launcher
(CPUs 0-3 on the tested H700), rather than imposing a new fixed mask.

The obvious alternative, correcting `23` to `2,3`, was tested with the same
frontend/core/content in an ABBA Tomak replay. All ten observed threads moved
to CPUs 2-3, including audio/video and Mali workers. All runs were guest/source
audio and screenshot exact, with no ALSA errors, and sampled at 1,512 MHz.
Results for the last 600 of 900 replay frames:

| Run | Allowed CPUs | Core mean (ms) | Interval p99 (ms) | Interval max (ms) | Intervals over 20 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| A1 | 0-3 | 8.018 | 17.473 | 23.774 | 4 |
| B1 | 2-3 | 8.070 | 17.972 | 18.221 | 0 |
| B2 | 2-3 | 8.055 | 17.924 | 18.155 | 0 |
| A2 | 0-3 | 8.067 | 17.371 | 22.918 | 2 |

The two-core limit reduced the longest intervals but increased p99, with no
qualified core speedup. Midstream all-zero stereo-frame counts from the ALSA
observer also rose: 201/213 in A versus 1,430/1,388 in B across the same eight
48,000-frame windows (bins 6-13). This heuristic cannot establish an audible
dropout or its cause, but it does not support adopting a two-core audio-quality
improvement. The two-core candidate was superseded by the small pinning bypass;
it is not the installed policy.

The final patch passed shell syntax and on-device scope checks. The original
helper is backed up at
`/mnt/SDCARD/gp32-dev/resume37-affinity/general_functions.before.sh`.
The installed helper SHA-256 is
`ae6ae624a51bb9a2bfac508d18507e69588f24e185c879f82a1b6743b16f6f24`.
Source and evidence: `F:/GP32/results/resume37-affinity/{abba.json,
final-manifest.json,installed-final.json}`. No governor, volume, game save,
core binary or RetroArch configuration was changed for this adjustment.

A final installed-function run loaded the production core, exited with code 0,
retained CPUs 0-3 and no longer emitted the affinity error. ALSA reported no
errors/recoveries. This was an integration check with private config/save paths,
not another performance or physical audio-quality comparison. Evidence:
`F:/GP32/results/resume37-final/{runtime-verified.json,active-status.txt,
frontend.exit,function.log}`.

## Optional diagnostic: distinguish FIFO padding from silent game audio

`69a4f0e-alsa-padding-trace.patch` applies after the FIFO wakeup correction to
the pinned RetroArch source. It is a diagnostic patch, not part of the
installed production frontend. Build it in a separate artifact and set
`GP32_ALSA_PADDING_LOG` to a writable private CSV path for one bounded run.

The playback worker records the FIFO's available bytes, real bytes consumed,
period bytes, explicitly zero-padded bytes and `snd_pcm_writei` result.
`monotonic_ns` is sampled before acquiring the FIFO mutex; it is not a core
frame timestamp or an input-latency measurement. The trace uses a 16,384-entry
array and writes the file only when the worker ends. Check `allocated=1` and
`overflow=0` in its first line before interpreting the full run. One worker
lifecycle overwrites the path; use a distinct file per run and do not infer
complete coverage after a driver restart or crash.

The H700 diagnostic was built and exercised with the existing 900-frame Tomak
replay. All 1,034 writes returned a full 768 frames, without errors; every
per-frame guest PC, source-audio count/nonzero count and IIS state matched the
standalone reference. The trace allocated successfully and did not overflow.
Within output frames 96,000 through 719,999 (a central 13-second window at
48 kHz), explicitly padded bytes were zero. Small all-zero PCM sample counts
therefore do not imply FIFO padding in this run. Padding did occur around
the beginning and end of this bounded session. This is not a physical
listening test or evidence about every game or the earlier two-core runs.

Evidence: `F:/GP32/results/resume38-audio/{padding.csv,padding-summary.json}`
and `F:/GP32/results/resume38-padding-audio-{runtime-verified,summary}.json`.
The installed frontend/core, platform config and emulator config retained
their pre-run hashes; MainUI returned normally.

## Period granularity experiment: mixed result, not adopted (resume41)

`69a4f0e-alsa-period-comparison.patch` is **diagnostic only**. It applies to
`audio/common/alsa.c` at the same pinned revision, with LF SHA-256
`c076c5fb9ed5a3587b36a8253aeeadcc0906a8f3fc7f48e66ff0a36f52845c0c`.
Use the LF patch procedure above. In an isolated frontend build, setting
`GP32_ALSA_TEST_PERIODS=16` requests 16 playback periods; an unset variable
or any other value retains the original four-period request. Capture is
unchanged. The existing buffer-time request is retained, but ALSA negotiates
the actual period count: verify both period and buffer sizes in the log.
This is not a production recommendation or a gp32emu core option.

The H700 accepted **eight** periods of 384 frames (8 ms at 48 kHz), compared
with four periods of 768 frames (16 ms). Both had the same 3,072-frame PCM
buffer and software FIFO. The padding trace capacity was raised from 4,096
to 16,384 events to cover the additional wakeups in a 40-second replay.
Only a diagnostic build carries this allocation, and only when tracing is
enabled; installed production binaries are unchanged.

Her Knights Korea was replayed for 2,400 combat frames in ABBA order using
one diagnostic frontend/core pair. The table covers the final 1,200 frames;
all sampled host clocks in these windows were 1,512 MHz.

| Run | Actual period | Interval p99 (ms) | Interval max (ms) | Intervals over 20 ms |
| --- | ---: | ---: | ---: | ---: |
| A1 | 16 ms | 31.893 | 32.055 | 34 |
| B1 | 8 ms | 24.016 | 24.131 | 62 |
| B2 | 8 ms | 24.020 | 24.091 | 73 |
| A2 | 16 ms | 31.902 | 32.102 | 39 |

Smaller periods reduced the longest stalls but increased the frequency of
intervals over 20 ms. Do not call this stable 60 Hz or adopt it from p99 alone.
Every replay frame matched A1's guest PC, cycle count, source PCM count,
nonzero count and IIS register. Each run accepted all 1,764,180 offered
stereo frames, had no ALSA errors/recoveries, and had zero explicit FIFO
padding in output frames [96,000,1,872,000), a central 37-second window.
All traces allocated without overflow; final screenshots were pixel-identical.
These checks do not measure physical input latency or speaker quality.

The source patches were checked against LF copies and reproduce the compiled
diagnostic source exactly. Frontend SHA-256:
`5956a71c374cb6ecb8ab99dc3411ca6bae9bf69e143d69c85dafc21a3a0e9646`.
Both temporary frontend source changes were restored after building. All
runs returned to MainUI with installed binary/configuration hashes unchanged.
Evidence: `F:/GP32/results/resume41-periods/{summary.json,build-manifest.json,
patch-check.json,*-padding.csv}` and `resume41-periods-*-runtime-verified.json`.
