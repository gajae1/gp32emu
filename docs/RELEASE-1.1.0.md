# GP32emu 1.1.0 update notes

Changes since **1.0.1**.

## Apps and controls

- Added [ES-DE launcher configurations](https://github.com/gajae1/gp32emu/blob/v1.1.0/docs/es-de/README.md) for Windows, Linux, macOS and Android.
- Added an experimental iPhone/iPad app with a game library, touch controls and
  gamepad support. After [pairing and VPN setup](https://github.com/gajae1/gp32emu/blob/v1.1.0/ios/README.md#set-up-jit),
  games prepare JIT in the app. The menu also offers **Play without JIT**.
- Added ten save-state slots per game and **Load latest state** to the Windows,
  Android and iOS apps.
- Improved mobile landscape layout and touch controls. The menu button hides
  during play, and hiding touch controls gives the picture more space.
- Fixed held input after disconnecting an Android controller. Other controllers
  and touch input stay active.
- Fixed unintended Windows audio playback after a pause or reset, and timing
  after a long pause.
- Improved iOS audio continuity during playback and after resuming a game.

## Games and saves

- Games on SmartMedia cards open faster. The loader reads the directories and
  files it needs directly from the card image.
- Fixed loading games and BIOS files and writing saves in Windows folders
  with Korean and other non-ASCII names.
- Added v17 save states that retain more of the audio and device state. Older
  supported states can still be loaded; older builds cannot read v17.
- Fixed audio and video restoration after loading states. Invalid states are
  rejected before they replace the running game.
- Fixed 128 MiB card saves, small SmartMedia card geometry and empty FAT files.
- Fixed error handling for malformed FXE headers and FXP2 data.
- Fixed browser save, load and export operations when switching games or slots.

## Emulation

- Updated ARM and Thumb instruction handling, memory access and interrupt
  behavior.
- Fixed interpreter fallback when JIT code cannot be made executable, and
  handling of changes to emulated program memory.
- Improved ARM64 and x86-64 execution, SmartMedia reads and state saving.
- Corrected DMA transfer order and PWM, LCD and audio timing.
- Updated audio filtering, frame delivery and output handling across frontends.
