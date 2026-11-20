#!/bin/sh
# Launch an APK headlessly in the booted simulator, perform some steps, and write the
# last frame to <work>/frame.png (<work> is $A2I_WORK or out/dev; log in <work>/vm.log).
#
#   launch.sh <apk> [seconds] [step]...
#
# Steps are those of a2i.boot.Launcher: tap:x,y  back  type:text  shot:file.png  wait:ms ...
# The screen is 1080x2340 at 420 dpi unless W, H and DPI say otherwise.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${A2I_WORK:-$root/out/dev}
mkdir -p "$work"
# The VM runs with the simulator's working directory, so paths must be absolute.
apk=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
secs=${2:-60}
shift
[ $# -gt 0 ] && shift

rm -f "$work/frame.png"
SIMCTL_CHILD_A2I_APP_LIBS=$("$here/applibs.sh" "$apk" 2>/dev/null) "$here/runvm.sh" "$secs" a2i.boot.Launcher \
    "$root/out/runtime/android-root" "$root/out/runtime/iphonesimulator/Frameworks" "$work/data" \
    "$apk" "$work/frame.png" "${W:-1080}" "${H:-2340}" "${DPI:-420}" "$@"
# The log without the lines every run prints.
grep -av "setting boot class\|Using Collector\|\.dm'\|Typeface: Pre\|SystemFonts: Load\|class_linker.cc:6\|PackageParser: Unknown element\|OverlayConfig\|a2i.Services: unimpl" \
    "$work/vm.log" | cut -c1-"${COLS:-240}" | head -"${LINES_MAX:-30}"
