# GP32emu 1.1.0

This release adds an experimental iPhone/iPad app, ten manual save-state slots
per game in the standalone apps, and updates to controls, audio and saving.

## New and improved

- **iPhone and iPad:** import BIOS and games, use touch controls or a controller,
  and play in portrait or landscape. The unsigned IPA requires a compatible
  signing tool. JIT requires separate activation.
- **Windows, Android and iOS:** ten manual save-state slots for each game. The
  previous single iOS state appears in slot 1.
- **Save states:** load the latest state for the current game after
  confirming its slot and date; no save slot is created or replaced
  automatically. On iOS, choose previously imported games from the game library
  without importing them again.
- **Mobile controls:** a top-aligned landscape picture, separate shoulder and
  Start/Select rows, a menu button that hides
  during play, and more screen space when touch controls are disabled.
- **Audio and emulation:** updates to CPU, DMA, timer and audio handling. Save
  states retain all four PCM channels and their playback positions.
- **Loading and saving:** damaged or mismatched states are rejected without
  replacing the running game. Restored states include the saved audio state.

## Updating

Install over the existing application or replace the matching RetroArch core.
Keep your BIOS, games, settings and saves. Do not uninstall a mobile app first.
On iOS, use the same signing account and app identifier when updating.

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
