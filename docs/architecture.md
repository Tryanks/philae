# Architecture

Philae has two halves: `philae`, a packer that runs on a Mac and turns an APK into an IPA,
and **a2i** ("Android to iOS"), the runtime that ships inside every IPA and makes the app
believe it is on Android.

```
            build time                                run time (one iOS process)
 ┌───────────────────────────┐        ┌───────────────────────────────────────────────┐
 │ philae pack app.apk       │        │ A2IHost (UIKit)      touches, keyboard, frames │
 │  · read manifest, icon    │        │ ─────────────────────────────────────────────  │
 │  · .so  →  .dylib         │  IPA   │ the app's dex code   the app's converted .so   │
 │  · bundle host + runtime  │ ─────▶ │ Android framework    bionic shim, Android-ABI  │
 │  · sign                   │        │ in-process services  JNI                       │
 └───────────────────────────┘        │ ART, libcore, ICU, BoringSSL (interpreter)     │
                                      └───────────────────────────────────────────────┘
```

## What iOS allows, and what follows from it

Three platform rules shape the design.

**No executable memory at run time.** A device refuses `mmap` and `mprotect` requests for
executable pages, and every executable page must be covered by a code signature. So there
is no JIT, dex code cannot be compiled on the device, and an Android `.so` cannot be
loaded by a custom ELF loader. Bytecode is therefore interpreted, and native libraries
are translated into real, signed Mach-O dylibs when the IPA is built.
`tests/device-limits` enforces the same rule in the simulator, where it is otherwise
absent, to catch violations without a device.

**One process.** There is no `fork`, no `exec`, and no other process to talk to. Android
is built around a separate `system_server` reached over Binder, so every system service
an app uses has to live inside the app's own process.

**No Linux.** The kernel interfaces that Android's native code is written against
(futexes, epoll, `/proc`, ashmem, signals with Linux semantics) are missing or different,
and Apple's arm64 calling convention differs from the standard one in how it passes
variadic and stack arguments.

## The packer (`packer/`)

`philae pack` does four things.

1. **Reads the APK**: the binary XML manifest (`axml.rs`, `manifest.rs`) and the resource
   table (`arsc.rs`), to find the package name, label, launcher activity and icon. The
   icon is rendered to the sizes iOS wants (`icon.rs`).
2. **Converts native libraries** (`elf2macho.rs`): each `lib/arm64-v8a/*.so` becomes a
   Mach-O dylib. The machine code is not recompiled. The ELF image is kept at the same
   relative layout inside Mach-O segments, its relocations are re-expressed as dyld rebase
   and bind opcodes, and its exports go into an export trie. Imports are bound by name to
   the bionic shim (`bionic_<name>`) or to sibling libraries of the same app. Two things
   in the code itself need patching: reads of the thread pointer (`mrs xN, tpidr_el0`,
   which Android uses for the stack guard and thread slots) are redirected to stubs that
   return a compatible block, and initializers are registered where dyld will run them.
3. **Assembles the bundle** (`bundle.rs`): the prebuilt host executable, the runtime's
   dylibs, the Android root (boot class path, framework, ICU and time zone data, fonts,
   certificates), the APK itself, and an `Info.plist` derived from the manifest.
4. **Signs** (`sign.rs`): ad hoc for the simulator; with a development identity and a
   provisioning profile for devices.

## The runtime, a2i (`runtime/`)

### ART and the core libraries (`runtime/aosp`, `runtime/vm`)

ART, libcore's native code, ICU, BoringSSL and Conscrypt are built from AOSP sources at a
pinned release tag, with CMake instead of AOSP's build system. Changes to AOSP code are
kept as one patch per repository in `runtime/aosp/patches/`; code that has no Darwin
counterpart upstream is in `runtime/aosp/art-ios/` (futexes on `os_sync_wait_on_address`,
the fault handler, signal chaining, native stack traces).

The VM is configured for what iOS permits: the portable C++ interpreter only (ART's
assembly interpreter and both compilers are off), no boot image, and the concurrent
mark-sweep collector, which needs no special kernel support. ART wants its heap in the
low 4 GiB of the address space, which iOS does not offer; the patched runtime instead
finds a free 4 GiB-aligned window and treats that as its base.

The boot class path (core-oj, core-libart, okhttp, bouncycastle, conscrypt, ICU4J) comes
from an Android 15 system image unchanged.

### The framework (`scripts/build-framework.py`, `runtime/framework`)

The Android framework's Java code is AOSP 15's, unmodified, taken as class files from
Robolectric's `android-all` distribution and compiled to dex. This is what makes
unmodified apps plausible: `Activity`, `View`, `Resources`, the layout inflater and
everything else behave exactly as on a device because they are the same code.

The framework's native half is supplied three ways:

- `libandroid_runtime` (Skia, `android.graphics`, text layout, the resource system) is
  AOSP's own host build for macOS/arm64, which ships inside `android-all`. Its Mach-O
  headers are rewritten so that iOS loads it (`scripts/prepare-prebuilt-runtime.sh`).
  This is scaffolding; the proper replacement is building the same sources for iOS.
- Classes whose native code is tied to Linux or to Binder are reimplemented in
  `runtime/framework`: `MessageQueue` (a condition variable instead of epoll), `Binder`
  and `Parcel` (in-process, with objects kept in a side table), `SystemProperties`,
  `SystemClock`, the display vsync source, audio and media stubs.
- Everything else is registered lazily and fails loudly when first called.

### System services (`runtime/java/a2i/boot`)

Framework classes obtain services by name from the service manager and call them through
AIDL interfaces. Philae replaces the service manager with a registry of local objects
implementing those same interfaces, so a "remote" call is a method call (`Services.java`).

A service implements only the methods that matter. A `java.lang.reflect.Proxy` answers
every other method with a neutral value (null, zero, false, an empty list) and logs the
first call. That log is the main development tool: it lists what an app asked for that
nobody answered. Services nobody has declared at all get the same treatment on demand.

The larger ones are the activity manager (`SystemServices.java`), the package manager,
which answers from the parsed APK (`PackageService.java`), notifications, input methods,
connectivity, storage, and content providers for settings and documents.

### Activities, services, receivers (`runtime/java/android/app`)

Nothing about the app's lifecycle is simulated. The framework's own `ActivityThread` runs
in the process, and Philae plays the part of the system: it sends the same
`ClientTransaction` items a device would (`LaunchActivityItem`, `ResumeActivityItem`,
`PauseActivityItem`, ...) and the same `scheduleCreateService` / `scheduleReceiver`
calls. `A2IActivityTaskManager` keeps the task's back stack and handles results, new
intents, `recreate()` and finishing. Intents that leave the app (links, sharing) go to
iOS.

### Windows and drawing (`runtime/java/android/view`)

`WindowManagerGlobal` is replaced. Each window's view hierarchy is driven by a small
`ViewRootImpl` stand-in (`A2IViewRoot`) that measures, lays out and draws it into a
bitmap with Skia's software renderer; `A2IWindowManager` composites the windows
(activities, dialogs, popups, toasts) into one frame, dispatches input to the right one
and delivers window insets. There is no SurfaceFlinger and no hardware acceleration.

### The iOS host (`runtime/host`)

`A2IHost` is the executable every converted app launches. The iOS main thread owns UIKit;
a second thread is Android's main thread and runs the VM and Android's message loop.
Between them:

- frames go to a `CALayer` as images at the screen's native scale;
- touches become `MotionEvent`s; an edge swipe is the back key;
- the iOS keyboard is driven through a hidden text field and connected to Android's
  `InputConnection`, so composition (for example pinyin) works;
- foreground and background transitions become activity lifecycle changes;
- `android.webkit.WebView` is implemented on `WKWebView`, placed as a native view over
  the Android frame, with cookies kept in sync (`runtime/java/a2i/webkit`).

### Native code from the app (`runtime/bionic`, `runtime/aosp/art-ios/android_abi_jni.*`)

A converted library is Android code in a Mach-O container: it still expects bionic and
the standard arm64 calling convention.

`liba2i_bionic` provides the C library and NDK functions it imports. Functions whose
behaviour and types match Darwin's are forwarded directly. The rest are translated:
`errno` values, `open` and `mmap` flags, `struct stat`, `dirent`, `FILE` and the standard
streams, pthread objects (bionic's are smaller than Darwin's, so they hold handles), and
variadic functions such as `printf`, whose argument lists are rebuilt in the layout the
caller used.

JNI has the same problem in the other direction. When ART calls a native method that
lives in a converted library, it passes arguments the Android way and hands it a shadow
`JNIEnv` whose function table adapts each entry, including the variadic `Call*Method`
family, back to ART's own. Converted libraries are recognised by address, from a marker
section the converter adds.

## Building it

`scripts/build-all.sh` runs the steps in order; each is a separate script with a header
describing it. Everything downloaded goes to `third_party/` and everything built to
`out/`; neither is part of the repository. To change AOSP code, edit the checkout under
`third_party/aosp/<repo>` and regenerate the patch:

```sh
git -C third_party/aosp/art diff > runtime/aosp/patches/art.patch
```
