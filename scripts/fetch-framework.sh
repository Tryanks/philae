#!/bin/sh
# Download the Android framework build the runtime uses: Robolectric's distribution of
# AOSP 15, published on Maven Central.
#
#   android-all                 the framework's classes, its resources, and (for desktop
#                               hosts) the matching native code, libandroid_runtime
#   nativeruntime-dist-compat   system fonts and their configuration
#
# scripts/build-framework.py turns these into the runtime's framework files, and
# scripts/prepare-prebuilt-runtime.sh makes the native library loadable on iOS.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/third_party/robolectric"
maven=https://repo1.maven.org/maven2/org/robolectric
mkdir -p "$dest"

fetch() {  # fetch <file> <url>
    [ -f "$dest/$1" ] && { echo "have $1"; return; }
    echo "fetch $1"
    curl -fL --retry 3 -o "$dest/$1.part" "$2" && mv "$dest/$1.part" "$dest/$1"
}
fetch android-all-15.jar "$maven/android-all/15-robolectric-14663667-2/android-all-15-robolectric-14663667-2.jar"
fetch nrt.jar "$maven/nativeruntime-dist-compat/1.0.20/nativeruntime-dist-compat-1.0.20.jar"
