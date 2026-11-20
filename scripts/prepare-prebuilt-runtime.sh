#!/bin/sh
# Make the prebuilt macOS build of Android's host runtime loadable on iOS.
#
#   scripts/prepare-prebuilt-runtime.sh [iphonesimulator|iphoneos]   (default: both)
#
# libandroid_runtime is AOSP's own host build of the framework's native code (Skia and the
# android.graphics JNI layer, text layout, fonts, input event classes), published inside
# Robolectric's android-all jar together with the framework classes it was built for. It is
# arm64 code that only uses libSystem, libc++, libobjc and CoreFoundation, all of which
# iOS has too, so it runs once its load commands name the right platform and its two
# macOS-only framework references are redirected: CoreServices to
# runtime/simulator/macshim.c, and AppKit, from which it uses nothing (and which may not
# share a name with the other, dyld rejects duplicates), to libobjc.
#
# This is scaffolding for bringing the framework up, not a supported way to ship: the
# real thing is to compile the same AOSP sources for iOS.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
jar="$root/third_party/robolectric/android-all-15.jar"

prepare() {  # prepare <sdk> <vtool platform>
    frameworks="$root/out/runtime/$1/Frameworks"
    lib="$frameworks/libandroid_runtime.dylib"
    [ -f "$frameworks/liba2i_macshim.dylib" ] || { echo "build the runtime first (scripts/build-runtime.sh)"; exit 1; }
    tmp=$(mktemp -d)
    unzip -o -q "$jar" native/mac/aarch64/libandroid_runtime.dylib -d "$tmp"
    xcrun vtool -set-build-version "$2" 17.4 "$(xcrun --sdk "$1" --show-sdk-version)" \
        -replace -output "$lib" "$tmp/native/mac/aarch64/libandroid_runtime.dylib" 2>/dev/null
    rm -rf "$tmp"
    xcrun install_name_tool \
        -change /System/Library/Frameworks/AppKit.framework/Versions/C/AppKit /usr/lib/libobjc.A.dylib \
        -change /System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices @rpath/liba2i_macshim.dylib \
        -change /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation \
                /System/Library/Frameworks/CoreFoundation.framework/CoreFoundation \
        "$lib" 2>/dev/null
    codesign --force --sign - "$lib"
    echo "$lib"
}

for sdk in ${1:-iphonesimulator iphoneos}; do
    case "$sdk" in
        iphonesimulator) prepare iphonesimulator iossim ;;
        iphoneos) prepare iphoneos ios ;;
        *) echo "unknown SDK $sdk"; exit 1 ;;
    esac
done
