#!/bin/sh
# Build the runtime host for device and simulator into out/runtime/<sdk>/A2IHost.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)

for sdk in iphoneos iphonesimulator; do
    build="$root/out/cmake/$sdk"
    cmake -S "$root/runtime" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=17.4 \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$root/out/runtime/$sdk" >/dev/null
    cmake --build "$build"
done
