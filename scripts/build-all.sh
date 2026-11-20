#!/bin/sh
# Fetch and build everything the packer needs, in dependency order. Each step can also be
# run on its own; all of them skip downloads that are already present.
#
# Needs Xcode, CMake, Ninja, a JDK, Python 3, Rust and 7-Zip (7zz). Downloads about 2.5 GB.
set -eu
cd "$(dirname "$0")/.."

./scripts/fetch-aosp.sh             # AOSP sources for ART, libcore, ICU, ... (patched)
./scripts/fetch-android-image.sh    # boot class path, ICU and time zone data
./scripts/fetch-framework.sh        # framework classes, resources, fonts, graphics library
./scripts/build-java.sh             # this project's Java code (also fetches D8)
python3 -I scripts/build-framework.py  # framework.jar, framework-res.apk, fonts
./scripts/build-runtime.sh          # native runtime for device and simulator
./scripts/prepare-prebuilt-runtime.sh  # the graphics library (prebuilt, relabelled)
cargo build --release               # the packer: target/release/philae
