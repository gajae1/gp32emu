# Building the iOS app

For installation and controls, see the [iPhone and iPad guide](../ios/README.md).
These commands build the shared GP32 core and UIKit frontend. Use **Release**
for performance checks; an unoptimized Debug build is not representative.

Device builds include the experimental AArch64 JIT by default, but start with
the interpreter. Configure `-DGP32EMU_IOS_DEVELOPER_JIT=OFF` to omit it.
Simulator and portable host builds use the interpreter.

## Build on a Mac

Install Xcode with an iOS SDK and CMake 3.21 or later. Open Xcode once to finish
its setup. Run these commands from the repository root.

For the simulator:

```sh
cmake -S ios -B build-ios-sim -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
cmake --build build-ios-sim --config Debug --target gp32emu_ios --parallel 2
open build-ios-sim/gp32emu_ios.xcodeproj
```

Select an installed iPhone or iPad simulator in Xcode and run `gp32emu_ios`.
A simulator does not need a device signing identity.

For an iPhone or iPad:

```sh
cmake -S ios -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
open build-ios/gp32emu_ios.xcodeproj
```

Choose your signing team and an available bundle identifier in the app target's
Signing & Capabilities settings. Connect and unlock the device, approve its
computer-trust prompt, and enable Developer Mode when prompted. Select the
device in Xcode and run the app. These steps require interaction on the Mac and
device; a source archive does not provide signing credentials.

With signing configured, a device build can also use:

```sh
xcodebuild -project build-ios/gp32emu_ios.xcodeproj -scheme gp32emu_ios \
    -configuration Release -destination 'generic/platform=iOS' -jobs 2 build
```

To create an unsigned IPA for sideloading, use a separate build directory:

```sh
cmake -S ios -B build-ios-sideload -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
xcodebuild -project build-ios-sideload/gp32emu_ios.xcodeproj \
    -scheme gp32emu_ios -configuration Release -sdk iphoneos -jobs 2 \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
stage=$(mktemp -d)
mkdir -p "$stage/Payload"
ditto build-ios-sideload/Release-iphoneos/GP32emu.app "$stage/Payload/GP32emu.app"
test "$(plutil -extract CFBundleIdentifier raw "$stage/Payload/GP32emu.app/Info.plist")" = org.gp32emu.ios || exit 1
if codesign -d "$stage/Payload/GP32emu.app" >/dev/null 2>&1; then
    codesign --remove-signature "$stage/Payload/GP32emu.app"
fi
rm -f "$stage/Payload/GP32emu.app/embedded.mobileprovision"
output="$PWD/gp32emu-1.1.0-ios-unsigned.ipa"
test ! -e "$output" || { echo "Choose a new output filename; the IPA already exists."; exit 1; }
(cd "$stage" && zip -qry "$output" Payload)
```

The output check prevents `zip` from updating an old archive and retaining
obsolete files. Use a new filename if an IPA already exists. Never publish the
locally signed device bundle: signing and provisioning data belong only in
private test builds. The staging directory printed by `echo "$stage"` can be
removed after packaging.

## Developer debugger workflow

For a locally signed, debuggable device build, load a game and attach Xcode's
LLDB to GP32emu. At the stopped debugger prompt:

```text
command script import /absolute/path/to/gp32emu/ios/tools/jit.py
gp32-jit-prepare
```

The process resumes. After the app reports that memory is prepared, interrupt
it, then run `gp32-jit-enable`. The helper initializes the complete code region
through the debugger and resumes the app to activate JIT on the core's owner
queue. Interrupt it again and run `gp32-jit-status` to confirm “JIT enabled.”,
then use `process detach` to resume without the debugger. The game menu also
shows the current execution mode in developer JIT builds.

Preparation or activation failure keeps the interpreter available. Run
`gp32-jit-prepare` again before retrying activation; an old prepared address
must not be reused. Repeating either command when JIT is already active leaves
its code memory unchanged. Repeat the handshake
after restarting the app or changing games; reset and state restore retain the
current grant while invalidating translated guest code.

The LLDB workflow has run Blue Angelo and Astonishia Story R on an iPhone with
matching interpreter/JIT video, PCM and final save-state output. This does not
establish full compatibility across devices or the game library.

## Host checks without an iOS SDK

Build the portable C bridge on Windows, Linux or macOS:

```sh
cmake -S ios -B build-ios-host
cmake --build build-ios-host --target gp32emu_ios_bridge --parallel 2
```

Run the focused reset, state-restore and audio-transition regression:

```sh
cmake -S . -B build-tests -DGP32EMU_BUILD_TESTS=ON
cmake --build build-tests --target ios_bridge_test --parallel 2
ctest --test-dir build-tests -C Debug -R '^ios_bridge$' --output-on-failure
```

The host regression passed on Windows and Apple Silicon macOS. It checks the portable C code, not the
UIKit app or actual audio playback. The core selects C23 when supported and
otherwise falls back to C11.
