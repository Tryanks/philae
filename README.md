# Philae

Run Android apps on iOS. Philae repackages an APK into an IPA that carries its own Android
runtime: the app's bytecode runs on ART, against the real Android framework, inside an
ordinary iOS app process.

```sh
philae pack app.apk --launch          # build an IPA and start it in the iOS Simulator
```

It is a compatibility layer, not an emulator: nothing emulates a CPU or boots Android.
The APK's dex code is interpreted by a port of ART, the app's arm64 native libraries are
converted to iOS dylibs ahead of time, and the Android system services the framework
expects are provided in-process and bridged to UIKit. The closest relative is Wine.

Philae has two parts. `philae` is the packer, a command-line tool that runs on a Mac.
**a2i** ("Android to iOS") is the runtime that ships inside every converted app.

The name comes from the Philae obelisk, the bilingual inscription that was read alongside
the Rosetta Stone, and from the lander that the Rosetta spacecraft set down on a comet.

## Status

Experimental. It runs real, unmodified apps, including ones built with Jetpack Compose and
ones that ship native libraries, and it is far from running arbitrary ones. It has been
exercised in the iOS Simulator and, less thoroughly, on an iPhone.

| Area | State | Notes |
| --- | --- | --- |
| Dex bytecode (Java, Kotlin, coroutines) | Works | Interpreted; no JIT or ahead-of-time compilation |
| Activities, fragments, back stack, results, `recreate()` | Works | Driven by the framework's own `ActivityThread` |
| Views, AppCompat, Material components, Jetpack Compose | Works | Software rendering |
| Resources, themes, configuration, fonts, locales | Works | |
| Dialogs, popups, menus, toasts | Works | |
| Touch input, back gesture | Works | Single touch; edge swipe is the back key |
| Soft keyboard, text composition | Works | The iOS keyboard, through `InputConnection` |
| Files, SharedPreferences, SQLite, DataStore | Works | Inside the app's container |
| Networking, TLS | Works | Android's own stack and trust store |
| Arm64 native libraries (JNI, NDK C library) | Partial | Converted at build time; no ELF TLS or packed relocations; functions the C library shim lacks abort when called |
| Services, broadcast receivers, content providers | Partial | In-process only; no background execution |
| `WebView` | Partial | `WKWebView` underneath; no `shouldInterceptRequest`, JavaScript interfaces cannot return values, drawn above dialogs |
| Opening links, sharing, document picking | Partial | Links and the share sheet go to iOS; folder picking is answered automatically |
| Notifications | Missing | Accepted and ignored |
| Audio playback | Partial | `MediaPlayer` for audio files |
| Video, `MediaCodec`, `AudioTrack`, `MediaSession` | Missing | |
| OpenGL ES, Vulkan, hardware-accelerated drawing | Missing | |
| Multi-touch | Missing | |
| Camera, location, sensors, Bluetooth, NFC | Missing | Network and battery state are fixed answers |
| Installing packages, split APKs, app bundles | Missing | One APK, one process |
| Google Play services, other apps' components | Missing | |
| Spawning executables | Not possible | iOS does not allow it |

An IPA is about 120 MB larger than its APK, and needs iOS 17.4 or later on arm64.

### Roadmap

Roughly in the order they unblock the most apps:

1. Media: `AudioTrack`, `MediaCodec` on VideoToolbox/AudioToolbox, `MediaSession`,
   video surfaces.
2. Graphics: build the framework's native graphics code from source for iOS instead of
   relabelling the prebuilt host library; then a GPU path (OpenGL ES over Metal).
3. Performance: ahead-of-time compilation of dex code at pack time, so that less runs in
   the interpreter.
4. Native libraries: ELF thread-local storage, packed relocations, wider C library and
   NDK coverage.
5. Platform bridges: multi-touch, notifications, camera, location, sensors, real
   connectivity and battery state.
6. Packaging: split APKs and app bundles, loading additional APKs at run time, a
   smaller runtime payload.
7. Device hardening: systematic testing on real hardware.

## Requirements

- An Apple Silicon Mac with Xcode and its iOS SDK (developed with Xcode 27 / iOS 27 SDK).
  Converted apps need iOS 17.4 or later.
- CMake, Ninja, a JDK (17 or later), Python 3, Rust, and 7-Zip:
  `brew install cmake ninja openjdk sevenzip rustup`
- About 10 GB of disk space and 2.5 GB of downloads for the first build.

## Building

```sh
./scripts/build-all.sh
```

This fetches the pinned AOSP sources, an Android 15 system image and the framework,
applies the patches in `runtime/aosp/patches/`, builds the runtime for the simulator and
for devices, and builds the packer at `target/release/philae`. Each step is a script of
its own under `scripts/` and can be rerun alone.

## Usage

```sh
philae inspect app.apk                      # what the packer sees: package, label, native code
philae pack app.apk --launch                # simulator: build, install and launch
philae pack app.apk --target device \
    --bundle-id com.example.myapp --install --device <device id>
```

Device builds are signed with a development identity from your keychain and need a
provisioning profile for the bundle identifier. `scripts/make-dev-profile.sh` has Xcode
create one; read its header first, since doing so registers an App ID with your Apple
developer account.

Only convert apps you have the right to use this way. An IPA built by Philae contains the
app and third-party runtime components; see [NOTICE](NOTICE) before distributing one.

## How it works

`philae pack` reads the APK's manifest and resources for the app's name, icon and
launcher activity, converts its native libraries from ELF to Mach-O, and assembles an
`.app` bundle around a2i: a prebuilt host executable and the Android runtime.

When the app starts, the host creates the VM, and Android's own `ActivityThread` launches
the APK's activities exactly as it would on a phone. The pieces underneath it:

| Layer | Where it comes from |
| --- | --- |
| ART, libcore, ICU, BoringSSL, Conscrypt | Built from AOSP sources with a small set of patches and Darwin ports (`runtime/aosp`) |
| Framework classes and resources | AOSP 15, unmodified, as distributed by Robolectric |
| System services (activity, package, window, input method, ...) | Implemented in-process in Java (`runtime/java`) |
| Window system and compositor | Replaced: windows are drawn in software and handed to UIKit as frames |
| C library and NDK for the app's native code | A shim exposing bionic's ABI on top of Darwin's libc (`runtime/bionic`) |
| Touch, keyboard, back gesture, lifecycle, sharing, web views | Bridged to UIKit and WebKit (`runtime/host`) |

[docs/architecture.md](docs/architecture.md) describes each part and the reasons behind it.

## Repository layout

```
packer/     the philae command-line tool (Rust): APK parsing, ELF to Mach-O, bundling, signing
runtime/    a2i: everything that ships inside a converted app
  aosp/       build of ART and friends from AOSP sources; patches and Darwin ports
  bionic/     Android C library compatibility for app native code
  framework/  native halves of framework classes (Binder, Parcel, MessageQueue, ...)
  java/       in-process system services, activity and window management, web view
  host/       the iOS app: UIKit view, input, keyboard, WKWebView
scripts/    fetch and build steps; scripts/dev has tools for headless testing
tests/      VM smoke test, native-library ABI tests, device restrictions check
```

## Development

a2i can be driven without the iOS app around it, which is the fastest way to
work on it. With a simulator booted:

```sh
./scripts/dev/launch.sh app.apk 60 tap:540,1200
open out/dev/frame.png                      # the last frame the app drew

./scripts/dev/session.sh start app.apk      # or interactively:
./scripts/dev/session.sh do tap:540,1200 wait:2000 shot:after
./scripts/dev/session.sh stop
```

The log (`out/dev/vm.log`) names every system-service method an app called that is not
implemented yet (`a2i.Services: unimplemented ...`), which is usually where to look first
when an app misbehaves.

Tests:

```sh
cargo test --release                        # packer unit tests
./tests/native/run.sh                       # Android-ABI native code and JNI, in the simulator
./scripts/build-tests.sh                    # VM and core library smoke test:
./scripts/dev/runvm.sh 60 -cp "$PWD/out/tests/smoke.jar" a2i.smoke.Main && grep "ALL OK" out/dev/vm.log
```

## Contributing

Open to everyone, with no process and no conditions: issues, pull requests, forks,
experiments and ideas are all welcome, in whatever form suits you.

## License

MIT; see [LICENSE](LICENSE). The patches under `runtime/aosp/patches/` modify AOSP code
and carry its licenses; see [NOTICE](NOTICE).

Philae is not affiliated with Google or Apple. Android is a trademark of Google LLC.
