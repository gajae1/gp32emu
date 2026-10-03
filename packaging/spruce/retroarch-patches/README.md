# RetroArch 1.22.2 ALSA wait correction

This is an optional **frontend source patch for OS/RetroArch builders**. It is
not part of the gp32emu core build and is not installed by the Spruce package.
The tested handheld still runs its original RetroArch binary and settings.

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
