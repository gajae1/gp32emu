# GP32emu for iPhone and iPad

The experimental iOS app is included in **1.1.0**. It runs on
ARM64 devices with iOS/iPadOS 15 or later. Supply your own GP32 BIOS and games.

The package is `gp32emu-1.1.0-ios-unsigned.ipa`. It includes the
interpreter and an experimental AArch64 JIT. Games start with the interpreter;
installing the IPA does not automatically enable JIT.

The iOS app remains experimental; features and performance may vary by device,
iOS version, and signing setup.

## Install and update

1. Set up [SideStore](https://docs.sidestore.io/docs/installation/prerequisites)
   using its current installation guide.
2. Import the GP32emu IPA into SideStore and sign it with your own Apple account.
3. Open GP32emu and import your BIOS and an extracted `.smc`, `.fxe` or `.fpk` game.

No source build is needed to install an IPA. The package has no publisher
signature or provisioning profile and includes no BIOS, games or saves.
Free-account installations need periodic refreshes through the signing tool.

An unsigned IPA cannot be installed by opening the download alone. SideStore
requires a computer for initial setup (Windows, macOS or Linux); later installs
and refreshes can run on the device with its required Wi-Fi/VPN setup. On iOS 16
and later, development-signed apps also require
[Developer Mode](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device).
The interpreter does not remove these installation requirements.

To update, install over the existing app using the same signing account and app
identifier. Do not uninstall first: removing the app also removes its local data.
Back up progress before changing signing tools or identifiers; a differently
identified app may have separate storage.

## Games and saves

Use the app menu to import files and select a game. A BIOS is recommended for
commercial games; booting without one is limited.

**Game library** lists games already imported into the app. Select a game to
open it without importing another copy. **Load latest state** finds the newest
of that game's ten saved slots and asks you to confirm its slot and date. It
does not create or replace a state automatically.

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
slot is empty or occupied; saving replaces the chosen slot. An existing single
state from an earlier iOS build remains available as slot 1.

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

JIT translates GP32 instructions into native ARM64 code. The interpreter remains
available when JIT cannot be enabled; its performance depends on the device and
game. The menu reports the mode actually in use.

For the experimental automatic setup:

1. Set up [StikDebug](https://github.com/StikDebug/StikDebug), including its
   pairing and VPN requirements. Your signing tool must preserve `get-task-allow`.
2. Load a game in GP32emu and select **Enable JIT with StikDebug** in the menu.
3. Complete the StikDebug request and return to GP32emu. Confirm **JIT enabled**
   in the menu before assuming it is active.

Automatic setup requires a supported device with Apple's TXM memory protection;
the app checks compatibility before starting. Repeat activation after restarting
the app or changing games. Reset and state
restore retain an active JIT session. Missing tools or failed setup leave the
interpreter available. Automatic JIT setup is experimental and depends on the
device, signing setup, and StikDebug configuration. Activation does not
guarantee full speed in every game.

## Licenses

Third-party notices from `licenses/` and [CREDITS.md](CREDITS.md) are included in
the app. Project licensing is described in the main README.
