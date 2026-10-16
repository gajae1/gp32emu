# GP32emu for iPhone and iPad

The iOS app is part of the **unreleased 1.1.0 development version**. It runs on
ARM64 devices with iOS/iPadOS 15 or later. Supply your own GP32 BIOS and games.

The development package is `gp32emu-1.1.0-ios-unsigned.ipa`. It includes the
interpreter and an experimental AArch64 JIT. Games start with the interpreter;
installing the IPA does not automatically enable JIT.

## Install and update

1. Set up [SideStore](https://docs.sidestore.io/docs/installation/prerequisites)
   using its current installation guide.
2. Import the GP32emu IPA into SideStore and sign it with your own Apple account.
3. Open GP32emu and import your BIOS and an extracted `.smc`, `.fxe` or `.fpk` game.

No source build is needed to install an IPA. The package has no publisher
signature or provisioning profile and includes no BIOS, games or saves.
Free-account installations need periodic refreshes through the signing tool.
The complete SideStore installation workflow is not yet verified for this build.

An unsigned IPA cannot be installed by opening the download alone. SideStore
requires a computer for initial setup (Windows, macOS or Linux); later installs
and refreshes can run on the device with its required Wi-Fi/VPN setup. On iOS 16
and later, development-signed apps also require
[Developer Mode](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device).
The interpreter does not remove these installation requirements. If your signing
tool cannot sign in to Apple, resolve that with the tool before installing the IPA.

To update, install over the existing app using the same signing account and app
identifier. Do not uninstall first: removing the app also removes its local data.
Back up progress before changing signing tools or identifiers; a differently
identified app may have separate storage.

## Games and saves

Use the app menu to import files and select a game. A BIOS is recommended for
commercial games; booting without one is limited.

Imported files keep their filenames. Their contents determine the save identity:
different files with the same name have separate saves, while importing identical
bytes again reuses the existing identity. Original files remain unchanged.

| Data | App storage |
| --- | --- |
| BIOS and games | `Documents/GP32emu/BIOS` and `Documents/GP32emu/Games` |
| Save states | `Documents/GP32emu/States` |
| In-game card progress | `Library/Application Support/GP32emu/Cards` |

The Documents folders are available through the Files app. In-game card progress
is separate from save states and is kept in the app's private Library directory.
A damaged, mismatched or unreadable card save stops loading to protect progress.
If saving fails while changing games, the current game stays loaded and paused.
Resolve the storage error and retry; closing the app can discard unsaved progress.

## Controls

Portrait places touch controls below the picture. Landscape places the 4:3
picture at the top, L/R and the main controls at the sides, and Select/Start
below the picture. A is above and to the right of B. Slide across the D-pad for
diagonals. Rotating the device releases held touch input.

The ellipsis opens the menu. In landscape it disappears after three seconds;
tap its position at the upper center of the picture to open the menu again.
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

GP32emu requests `universal.js` automatically. This path currently requires a
device with Apple's TXM memory protection; the app checks support before starting.
StikDebug's own supported-device list does not by itself establish compatibility
with this integration. Setup times out after at most 30 seconds and may end
earlier if iOS withdraws background time.

Repeat activation after restarting the app or changing games. Reset and state
restore retain an active JIT session. Missing tools or failed setup leave the
interpreter available. A local Xcode/LLDB activation method is described in the
[build guide](../docs/IOS_BUILD.md#developer-debugger-workflow).

Interpreter/JIT output and repeated code-cache reuse have passed targeted checks
on a physical iPhone. The complete StikDebug/SideStore workflow, automatic setup
on non-TXM devices, broader gameplay and physical audio-accessory changes remain
unverified. JIT is experimental, not a guarantee of full speed in every game.

## Building and licenses

See [Building the iOS app](../docs/IOS_BUILD.md) for Xcode, unsigned IPA packaging
and host checks. Third-party notices from `licenses/` and [CREDITS.md](CREDITS.md)
are included in the app. Project licensing is described in the main README.
