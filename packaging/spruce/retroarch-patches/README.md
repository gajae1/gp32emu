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

From a separate checkout of that exact RetroArch revision, with the patch path
replaced by its absolute location:

```sh
git -c core.autocrlf=false -c core.eol=lf apply --check /path/to/69a4f0e-alsa-fifo-wakeup.patch
git -c core.autocrlf=false -c core.eol=lf apply /path/to/69a4f0e-alsa-fifo-wakeup.patch
```

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
upstream source also passed. **A complete patched RetroArch build and its real
ALSA playback have not been validated.** This targeted fix does not establish
that every access to the driver's `volatile thread_dead` or concurrent teardown
is free of races.

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
The remaining pacing work requires a rebuilt, patched frontend comparison;
this source patch is not evidence that real playback is fixed.
