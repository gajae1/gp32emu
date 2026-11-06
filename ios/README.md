# GP32emu for iPhone and iPad

The experimental iOS app is part of the **unpublished 1.1.0 candidate**; the
current public release is **1.0.1**. It runs on ARM64 devices with iOS/iPadOS 15
or later. Supply your own GP32 BIOS and games. Compatibility and performance
vary by device, iOS version and signing setup.

The `gp32emu-1.1.0-ios-unsigned.ipa` package includes the
interpreter and an experimental AArch64 JIT. Games start with the interpreter;
installing the IPA does not automatically enable JIT.

## Install and update

1. Set up [SideStore](https://docs.sidestore.io/docs/installation/prerequisites)
   using its current installation guide.
2. Import the GP32emu IPA into SideStore and sign it with your own Apple account.
3. Open GP32emu and import your BIOS and an extracted `.smc`, `.fxe` or `.fpk` game.

The IPA has no publisher signature or provisioning profile and includes no BIOS,
games or saves. Free-account installations need periodic refreshes through the
signing tool.

An unsigned IPA cannot be installed by opening the download alone. SideStore
requires a computer for initial setup (Windows, macOS or Linux); later installs
and refreshes can run on the device with its required Wi-Fi/VPN setup. On iOS 16
and later, development-signed apps also require
[Developer Mode](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device).

To update, install over the existing app using the same signing account and app
identifier. Do not uninstall first: removing the app also removes its local data.
Back up progress before changing signing tools or identifiers; a differently
identified app may have separate storage.

## Games and saves

Use the app menu to import files and select a game. A BIOS is recommended for
commercial games; booting without one is limited.

Select an imported game from **Game library** to play it again.

The app copies imported files into its own storage and leaves the originals
unchanged. Save data follows the imported file contents: importing identical
contents again reuses the existing save, while different contents create a
separate save. Imported games and save states are available through the Files
app. In-game card progress is managed separately from save states.

A damaged, mismatched or unreadable card save stops loading to protect progress.
If saving fails while changing games, the current game stays loaded and paused.
Resolve the storage error and retry; closing the app can discard unsaved progress.

## Save states

Each game has ten save-state slots. Save State and Load State show whether each
slot is empty or occupied; saving replaces the chosen slot. **Load latest
state** shows the most recent slot and its date for you to confirm. It does not
write a new save. An existing single state from an earlier iOS build remains
available as slot 1.

New states use the common v17 format. This build imports existing v2-v12 and
v14-v16 states. Older core builds cannot read v17 states, so keep a copy of
state files you may need with an older build. In-game card progress uses the
same `.sav` data as before; changing state slots does not convert card saves.

## Controls

Portrait places touch controls below the picture. Landscape places the 4:3
picture at the top, L/R and the main controls at the sides, and Select/Start
below the picture. A is above and to the right of B. Slide across the D-pad for
diagonals. Rotating the device releases held touch input.

Choose **Hide touch controls** when using a keyboard or controller. The picture
uses the space reserved for the controls, including the Start/Select row, while
keeping its 4:3 aspect ratio. **Show touch controls** restores the buttons; the
app remembers your choice.

The ellipsis opens the menu. In landscape it disappears after three seconds;
tap its position or the nearby area at the upper center of the picture to open
the menu again. Its touch target is larger than the visible icon.
Rotation briefly shows the icon. Save/load success messages disappear after
two seconds; errors stay visible.

| Input | Default controls |
| --- | --- |
| Keyboard | Arrows: D-pad; Z/X: A/B; A/S: L/R; Enter: Start; Shift: Select; Escape: menu |
| Gamepad | D-pad, A/B and shoulders; X: Start; Y or Options: Select; Menu: app menu |

If an audio device disconnects or system audio resets, the game pauses. Choose
Resume from the menu when ready. A menu that is already open stays paused.

## Experimental JIT

The menu reports whether JIT is active. If it cannot be enabled, the interpreter
remains available.

For the experimental automatic setup:

1. Set up [StikDebug](https://github.com/StikDebug/StikDebug), including its
   pairing and VPN requirements. Your signing tool must preserve `get-task-allow`.
2. Load a game in GP32emu and select **Enable JIT with StikDebug** in the menu.
3. Complete the StikDebug request and return to GP32emu. Confirm **JIT enabled**
   in the menu.

Automatic setup requires a supported device with Apple's TXM memory protection.
Repeat activation after restarting the app or changing games. Reset and state
restore retain an active JIT session.

## Building from source

To build an IPA from a source checkout, see the
[iOS build guide](https://github.com/gajae1/gp32emu/blob/main/docs/IOS_BUILD.md).

## Licenses

Third-party notices from [licenses/](../licenses/) and [CREDITS.md](CREDITS.md)
are included in the app. See the main README for
[project licensing](../README.md#credits-and-licensing).
