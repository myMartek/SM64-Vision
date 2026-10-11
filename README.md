# SM64 Vision

SM64 Vision brings a landmark 1996 3D platformer to Apple Vision Pro as a spatial
experience: not a flat picture pinned in front of you, but a world you can look into and
step inside.

It is a visionOS port of [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx), which in
turn builds on the [SM64 decompilation](https://github.com/n64decomp/sm64). The game's
renderer, audio and input were rewritten for the headset — Metal instead of OpenGL, a
stereo pair per eye from the live head pose, and a native SwiftUI launcher.

**Try it:** [join the TestFlight](https://testflight.apple.com/join/p2cHPRZd) ·
**Something broken?** [open an issue](https://github.com/myMartek/SM64-Vision/issues)

> This is a proof of concept. Large parts of the game have never been played through in a
> headset. Expect to find something wrong, and please say what and where.

## You supply the game data

This repository and the app contain no game content. You need a ROM image of the 1996 US
release, created yourself from your own cartridge:

| | |
|---|---|
| File | `baserom.us.z64` (unmodified, US) |
| MD5 | `20b854b239203baf6c961b850a4a51a2` |

The app asks for the file on first launch, checks the MD5, and copies it into its own
container. It stays on your device. EU, JP and Shindou ROMs do not work — sm64coopdx
supports the US version only.

## Five ways to play

- **2D Window** — the game on a floating screen, exactly as it always looked.
- **3D Window** — the same framing, given real depth. *(beta)*
- **3D Portal** — a window into the world. The camera sits on the glass, so moving your
  head lets you see sideways past the frame, the way a real opening behaves.
- **3rd-Person VR** — the frame falls away and the world fills your field of view. *(beta)*
- **First-Person VR** — you stand where the character stands. With PS VR2 Sense
  controllers your hands are where the controllers are: swing doors open, climb poles hand
  over hand, throw what you are holding.

Modes switch while the game is running, under Settings › Render mode. Stereo depth,
antialiasing and supersampling are live too.

**Controllers:** DualSense and other extended gamepads, the Nintendo 64 controller for
Switch, and PS VR2 Sense controllers. A paired keyboard (or the Mac's, in the simulator)
covers the basics: `W` `A` `S` `D` to walk, `Space` to jump, arrow keys for the camera.

## Building it yourself

### What you need

- A Mac with Apple Silicon
- Xcode with the visionOS platform installed (the project targets visionOS 26.0)
- [Homebrew](https://brew.sh), and from it:

```bash
brew install make coreutils xcodegen
```

`make` and `coreutils` are for the engine: its Makefile needs GNU Make 4.x and `gcp`,
and macOS ships Make 3.81. `xcodegen` generates the Xcode project, which is not committed —
[project.yml](project.yml) is the source of truth.

### Simulator

No Apple developer account and no signing needed.

```bash
git clone https://github.com/myMartek/SM64-Vision.git
```

```bash
cd SM64-Vision && xcodegen generate
```

```bash
xcodebuild -project MarioVision.xcodeproj -scheme MarioVision -configuration Debug -destination 'platform=visionOS Simulator,name=Apple Vision Pro' -derivedDataPath .build/dd build
```

Name a concrete simulator as above rather than the generic simulator destination: the
generic one also asks for an x86_64 slice, and the engine is built for arm64 only.

The first build takes several minutes: a build phase compiles the whole engine into
`sm64coopdx/build/us_xrsimulator/libsm64coopdx.a` before the app links against it. Later
builds are incremental. If the engine fails to compile, the full output is in
`sm64coopdx/build/coopdx-build.log`.

Install and launch it:

```bash
xcrun simctl boot "Apple Vision Pro"; xcrun simctl install booted .build/dd/Build/Products/Debug-xrsimulator/MarioVision.app; xcrun simctl launch booted de.martek.sm64vision
```

Instead of picking the ROM through the simulator's document picker, you can drop it
straight into the app's container:

```bash
DIR="$(xcrun simctl get_app_container booted de.martek.sm64vision data)/Library/Application Support/sm64coopdx"; mkdir -p "$DIR" && cp /path/to/baserom.us.z64 "$DIR/"
```

Logs, without tying the app's lifetime to your terminal:

```bash
xcrun simctl spawn booted log show --last 5m --predicate 'processImagePath CONTAINS "MarioVision"' --style compact
```

Or simply open `MarioVision.xcodeproj` in Xcode and press Run.

### Your own Vision Pro

A device build has to be signed with your team and needs a bundle id of your own. Put both
in `Config/Local.xcconfig` — the file is ignored by git, so it never conflicts with a pull:

```
DEVELOPMENT_TEAM = ABCDE12345
PRODUCT_BUNDLE_IDENTIFIER = com.example.sm64vision
```

Then regenerate the project and run it on the headset from Xcode:

```bash
xcodegen generate && open MarioVision.xcodeproj
```

The defaults live in [Config/Signing.xcconfig](Config/Signing.xcconfig). Run
`xcodegen generate` again whenever `project.yml` or either xcconfig changes, and after
pulling.

### Debug and Release differ

Debug builds leave out sm64coopdx's bundled example mods and DynOS packs
(`MARIOVISION_BUNDLE_OPTIONAL_MODS`). They are loaded one step per rendered frame at
startup, which costs minutes in the simulator and changes nothing about the vanilla game.
Release builds include them.

### Optional pieces

Lua 5.3 for visionOS is committed as a prebuilt static library, because sm64coopdx only
ships desktop binaries. To rebuild it from the upstream sources:

```bash
./scripts/build-lua-visionos.sh
```

The engine can be built on its own, which is the quickest way to see a compiler error in
full:

```bash
cd sm64coopdx && gmake TARGET_VISIONOS=1 VISIONOS_SDK=xrsimulator DISCORD_SDK=0 COOPNET=0 UPDATER=0 visionos -j8
```

Leave out `VISIONOS_SDK=xrsimulator` for the device library.

## Tests

Both run on the Mac, without a simulator or a headset.

```bash
./scripts/test-msl-shaders.sh
```

The Metal backend generates its shaders at runtime from the N64 colour combiner, so a
syntax error would otherwise only show on the headset, at the moment the game first draws
with that combiner. This generates the shader for every combiner the game uses and runs
each through the offline Metal compiler.

```bash
./scripts/test-hand-math.sh
```

Checks the arm solver behind the tracked controllers against forward kinematics. The
controllers do not exist in the simulator; the maths does not need them.

## Where things are

```
app/                       The visionOS app: SwiftUI launcher, renderer, ROM setup
  Sources/PortalRenderer.swift   stereo rendering for all five modes
  Sources/GameBridge.swift       Swift's side of the boundary to the engine
sm64coopdx/                The game engine (see Credits)
  Makefile.visionos        build branch: a static library instead of an executable
  src/pc/visionos/         the platform layer — Metal, AVFoundation audio,
                           GameController input, hand tracking, the portal maths
Config/                    signing identity, overridable locally
project.yml                XcodeGen definition of the Xcode project
scripts/                   Lua build and the two host tests
tests/                     sources of the host tests
```

Nearly all visionOS code sits in `sm64coopdx/src/pc/visionos/`. The game's own sources
are touched where the headset needs different behaviour — camera, HUD placement, doors,
screen transitions — and those changes are guarded by `TARGET_VISIONOS`.

### How app and engine fit together

The engine does not own a window. It draws into a render command encoder that the app
hands it, once per eye, which is what makes the stereo modes possible. The contract is in
[mariovision_platform.h](sm64coopdx/src/pc/visionos/mariovision_platform.h) and
[gfx_metal.h](sm64coopdx/src/pc/visionos/gfx_metal.h). Three things are worth knowing
before changing anything there:

- **`libsm64coopdx.a` is linked with `-force_load`, not `-l`.** sm64coopdx registers its
  ROM assets from `__attribute__((constructor))` functions. Out of a static archive the
  linker drops any object that only contributes a constructor; the asset tables then stay
  empty and the audio sequencer runs through them forever. The game looks frozen and
  nothing crashes.
- **SwiftUI runs on the main thread, the game on its own.** Anything that touches game
  state is queued and executed by `mariovision_ui_pump()` on the game thread.
- **sm64coopdx's own start menus are hidden** in favour of the SwiftUI launcher; in-game
  UI is untouched. `MARIOVISION_NATIVE_UI_DEFAULT` in
  [mariovision_ui.c](sm64coopdx/src/pc/visionos/mariovision_ui.c) brings them back.

Multiplayer is off for now (`COOPNET=0`): sm64coopdx ships libcoopnet and libjuice as
desktop libraries only.

## Releases

Development happens here. The [TestFlight](https://testflight.apple.com/join/p2cHPRZd)
builds are made on the maintainer's own build server, not by GitHub Actions: a push to
`main`, `develop` or a `visionos/*` branch tells that server which commit to build
([release-build.yml](.github/workflows/release-build.yml)), and it fetches the source
from here.

Signing certificates and the rest of the release tooling live on that server and are not
part of this repository. Pull requests from forks are never built there.

## Feedback

Bugs, and anything that feels wrong, belong in the
[issues](https://github.com/myMartek/SM64-Vision/issues). What helps most: which level you
were in, which render mode, what you were doing, and whether it happens every time.

## Privacy

No accounts, no tracking, no data collection. The app goes online for one thing only, and
only when you ask it to: downloading the optional HD texture pack from the Mods tab.

## Credits and legal

- `sm64coopdx/` started as a copy of
  [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx) at commit `8cd6e59`, without its
  history — the changes go far enough that staying mergeable was never realistic. Its
  authors are listed in [sm64coopdx/credits.txt](sm64coopdx/credits.txt). Removed from the
  tree: the Discord Game SDK, prebuilt coopnet and Lua binaries for other platforms, the
  updater and the IDO compiler for N64 builds.
- sm64coopdx builds on the [SM64 decompilation](https://github.com/n64decomp/sm64) by the
  n64decomp team.
- The optional HD textures are the Render96 HD Texture Pack, the work of the
  [Render96](https://github.com/Render96) team.

SM64 Vision is not affiliated with, endorsed by, or connected to Nintendo. Super Mario 64
and all related names and characters are trademarks of Nintendo. No ROM and no assets
extracted from one are part of this repository.
