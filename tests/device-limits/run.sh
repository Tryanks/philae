#!/bin/sh
# Run the command-line VM in the booted simulator under a real device's executable-memory
# rules (see device_limits.c), and report what it would have been refused.
#
#   tests/device-limits/run.sh <log file> <a2i-vm arguments...>
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
lib="$root/out/tests/libdevice_limits.dylib"
mkdir -p "$root/out/tests"
xcrun --sdk iphonesimulator clang -target arm64-apple-ios17.4-simulator -dynamiclib -O1 -Wall \
    -o "$lib" "$root/tests/device-limits/device_limits.c"
log=$1; shift
SIMCTL_CHILD_DYLD_INSERT_LIBRARIES="$lib" \
    xcrun simctl spawn booted "$root/out/runtime/iphonesimulator/a2i-vm" "$@" > "$log" 2>&1 || true
echo "denied requests for executable memory: $(grep -ac 'DEVICE-LIMIT: DENIED' "$log" || true)"
echo "large reservations:"
grep -a 'DEVICE-LIMIT: note' "$log" | sed 's/.*len=\([0-9]*\).*/\1/' | sort -n | uniq -c \
    | awk '{printf "  %d x %.1f GiB\n", $1, $2 / 1073741824}'
