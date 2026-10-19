# GP32emu 1.1.0

This release adds an experimental iPhone/iPad app, ten save-state slots per game
in the standalone apps, and fixes to emulation, audio and state restoration.

## New and improved

- **iPhone and iPad:** import BIOS and games, use touch controls or a controller,
  and play in portrait or landscape. The unsigned IPA requires a compatible
  signing tool. JIT requires separate activation.
- **Windows, Android and iOS:** ten save-state slots for each game. Existing
  slots remain available; the previous single iOS state appears in slot 1.
- **Mobile controls:** improved landscape layout, a menu button that hides
  during play, and more screen space when touch controls are disabled.
- **Audio:** smoother recovery after pauses, state loads and audio interruptions.
- **Emulation:** corrected CPU/JIT memory handling, DMA transfers and timer
  behavior. Save states preserve all four PCM channels and their playback positions.
- **Loading and saving:** better handling of damaged states and failed loads,
  preserving the running game when a state is rejected.

## Updating

Install over the existing application or replace the matching RetroArch core.
Keep your BIOS, games, settings and saves. Do not uninstall a mobile app first.

New state files use v17. GP32emu 1.1.0 reads supported v2–v12 and v14–v16 states,
but older cores cannot read v17. Back up old slots before overwriting them if
you may return to an older version. In-game card saves keep their existing format.
RetroArch continues to manage its own state slots.

## Downloads

Applications and cores are packaged separately for each platform. iOS and
Switch support is experimental. BIOS images, games and saves are not included.

See the [README](../README.md) for package selection, the
[Korean manual](MANUAL.ko.md) for controls and saves, and the
[iPhone/iPad guide](../ios/README.md) for IPA installation.
