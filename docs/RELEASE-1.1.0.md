# GP32emu 1.1.0 development notes

**1.1.0 is not publicly released.** The current public release is
[1.0.1](RELEASE-1.0.1.md). Android APK and unsigned iOS IPA candidates have been
built for testing; this does not replace the existing public downloads.

## iPhone and iPad

The new app supports ARM64 devices running iOS/iPadOS 15 or later. It imports
BIOS and game files, supports in-game card saves and save states, and provides
touch, keyboard and controller input. Portrait keeps controls below the picture;
landscape separates the shoulder buttons from Start/Select. The menu icon hides
during landscape play and can be opened by tapping its position.

The IPA is intended for SideStore or another compatible signing tool. It has no
publisher signature, provisioning profile, BIOS, games or saves. The complete
SideStore installation workflow remains unverified. See the
[iPhone/iPad guide](../ios/README.md) for installation, controls and storage.

Games start with the interpreter. An experimental ARM64 JIT is included; it
requires external activation and is not enabled merely by installing the app.
The menu can request StikDebug activation on supported TXM devices. Local
Xcode/LLDB activation is also available. Complete StikDebug app-to-app activation
and automatic setup on non-TXM devices remain unverified.

Targeted physical-device checks matched interpreter/JIT video, audio and complete
save-state output for Blue Angelo and Astonishia Story R, including repeated
JIT code-cache reuse. These checks do not establish compatibility or full speed
across the entire game library.

Audio and lifecycle changes include:

- Fading in the first recovered samples after an audio underrun or state change.
- Rebuilding playback after an audio-engine configuration change while preserving
  menu and pause state; disconnects and system audio resets pause the game.
- Preserving imported content selections when an app update relocates its container.
- Keeping the current game paused and its progress in memory if saving fails
  while switching games, so the save can be retried.
- Keeping errors visible while dismissing successful save/load messages.
- Avoiding large temporary save-state allocations on frontend worker stacks.

## Android

- Moves the menu button outside the game picture and touch controls.
- Prevents extra fingers or gestures begun on the controls from opening the menu.
- Keeps controls inside system bars, display cutouts and gesture areas.
- Uses a hardware-accelerated canvas on Android 8 and newer, reducing the time
  screen scaling occupies the emulation/audio thread. Android 6/7 keep software
  rendering.
- Queues the first audio samples before starting playback, avoiding an empty
  audio track while the first emulated frame is being prepared. The same path
  is used after pause, reset and state load.
- Redraws touch controls only when their pressed state changes, while continuing
  to process held-button and sliding input.

Layout and input checks have run on emulators and a physical Android phone.
Short Blue Angelo and Astonishia Story R title-scene measurements sustained
about 60 frontend intervals per second with no steady-state audio underruns.
The updated ASR startup check also recorded no startup underruns. These are
bounded measurements, not a guarantee of perfect audio or input latency in
all scenes and devices.

Install an updated APK over the existing app to preserve imported files and saves.
Do not uninstall first.

## Shared core

The interpreter and native JIT backends include corrections to ARM/Thumb
instruction behavior, exception handling, privileged register access and
instruction/data address translation. These are shared emulation fixes rather
than filename-dependent game workarounds.

Thumb word loads/stores and ARM SWP now handle unaligned addresses consistently
with ARMv4 word transfers, avoiding incorrect reads and writes into the next word.

JIT changes reduce compilation and dispatch work, avoid compiling one-use paths,
and reduce decoded-instruction storage. ARM64 memory access, conditional branches
and polling-loop handling retain checked fallbacks. Polling safety checks no
longer change translation state for a conditional instruction that does not run.
Apple ARM64 execution preserves the host frame pointer for helper calls.

File transfers and eligible homebrew decompression avoid redundant host work.
FXP loading preserves the original guest loader when host decompression fails;
GXB loading respects initialized and zero-initialized data placement. Audio
filters reuse identical coefficients without changing the output samples.
Performance gains vary with the game and host; no library-wide multiplier is claimed.

Existing compact card saves and the Astonishia Story R/Pinball Dreams audio
corrections remain available; they are not new features of 1.1.0.

## Save-state compatibility

New save states preserve separate instruction and data translation caches and
require 1.1.0 or a later compatible build. Older states remain importable, but
cannot describe separate cache histories. Exact continuation is not guaranteed
for unusual cases that rely on those old mappings. Keep the original state files
when upgrading. In-game card saves are unchanged.

## Remaining validation

- The full SideStore installation and StikDebug activation workflow.
- Broader mobile gameplay, physical keyboard/controller latency and sustained
  performance beyond the measured scenes.
- Physical headphone/Bluetooth changes and real system-audio resets on iOS;
  injected audio-notification checks have passed.
- Nintendo Switch runtime behavior and Android RetroArch integration.

See [Building](BUILDING.md), [Android builds](ANDROID_BUILD.md) and
[iOS builds](IOS_BUILD.md) for source builds. Each binary ZIP includes a manifest
identifying its source revision and file checksums.
