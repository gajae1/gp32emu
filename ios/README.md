# GP32emu for iPhone and iPad

The experimental iPhone/iPad app runs on ARM64 devices with iOS/iPadOS 15 or
later. Bring your own GP32 BIOS and games.

## Install and update

1. Download `gp32emu-1.1.0-ios-unsigned.ipa` from the
   [release page](https://github.com/gajae1/gp32emu/releases/tag/v1.1.0).
2. Sign and install it with a sideloading tool. For SideStore setup, follow its
   [installation guide](https://docs.sidestore.io/docs/installation/prerequisites).
   On iOS 16 or later, enable [Developer Mode](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device).
3. Open GP32emu and import your BIOS and an extracted `.smc`, `.fxe` or `.fpk` file.

The IPA contains no signing credentials, pairing file, BIOS, games or saves.
SideStore needs a computer for its initial setup. Refresh free-account
installations through the tool that signed the app.
For updates, install over the existing app with the same signing identity and
app identifier. Back up your saves first; uninstalling removes the app's data.

## Set up JIT

Built-in JIT requires iOS/iPadOS 17.4 or later and compatible hardware.
After setup, games activate JIT in GP32emu without switching to another JIT app.

1. Keep the app extension when signing and installing GP32emu. The signer must
   sign the app, extension and framework together and preserve `get-task-allow`.
2. On a computer, connect the device and open **Manage Pairing File** in a current
   [iloader](https://iloader.app). Export a remote pairing file for that device;
   a legacy USB pairing record will not work. In GP32emu, choose
   **JIT setup → Import pairing file** and select the exported file.
3. Install [LocalDevVPN](https://github.com/jkcoxson/LocalDevVPN), then choose
   **Connect LocalDevVPN** in **JIT setup**. Approve VPN and local network access
   if prompted. Leave the VPN connected for automatic activation when loading games.
4. Open a game. The menu shows **JIT enabled** when ready. **Check setup** can
   prepare and check the connection before you play.

If activation fails, the game stays paused. Choose **Retry JIT setup**, or
**Play without JIT** to use the interpreter for that game. After an iOS update,
try **Reset JIT support files** if preparation fails. Replace a pairing file the
device no longer accepts. Keep the pairing file private.

The app requests JIT again after a restart or game change. Pausing, resetting
or loading a state keeps the current JIT session. **Use StikDebug** is available
as an external alternative; the built-in helper cannot run inside LiveContainer.

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

## Building from source

To build an IPA from a source checkout, see the
[iOS build guide](https://github.com/gajae1/gp32emu/blob/main/docs/IOS_BUILD.md).

## Licenses

Third-party notices from [licenses/](../licenses/) and [CREDITS.md](CREDITS.md)
are included in the app. See the main README for
[project licensing](../README.md#credits-and-licensing).
