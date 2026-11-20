#!/bin/sh
# An interactive headless session with an APK in the booted simulator.
#
#   session.sh start <apk>    launch in the background
#   session.sh do <step>...   perform steps (see a2i.boot.Launcher), then print what the
#                             app logged about activities and errors. "shot:NAME" writes
#                             <work>/NAME.png and a reduced NAME_s.png
#   session.sh stop
#
# <work> is $A2I_WORK or out/dev; the VM's log is <work>/vm.log.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${A2I_WORK:-$root/out/dev}
mkdir -p "$work/data"

case "${1:-}" in
start)
    pkill -9 -x a2i-vm 2>/dev/null
    : > "$work/steps"
    # The VM runs with the simulator's working directory, so paths must be absolute.
    apk=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
    SIMCTL_CHILD_A2I_APP_LIBS=$("$here/applibs.sh" "$apk" 2>/dev/null) xcrun simctl spawn booted \
        "$root/out/runtime/iphonesimulator/a2i-vm" "$root/out/runtime/android-root" "$work/data" \
        a2i.boot.Launcher "$root/out/runtime/android-root" "$root/out/runtime/iphonesimulator/Frameworks" \
        "$work/data" "$apk" "$work/frame.png" "${W:-1080}" "${H:-2340}" "${DPI:-420}" \
        "follow:$work/steps" > "$work/vm.log" 2>&1 &
    sleep "${BOOT:-3}"
    ;;
do)
    shift
    for step in "$@"; do
        case "$step" in
        shot:*)
            name=${step#shot:}
            rm -f "$work/$name.png"
            echo "shot:$work/$name.png" >> "$work/steps"
            i=0
            while [ ! -f "$work/$name.png" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
            sips -Z "${SIZE:-600}" "$work/$name.png" --out "$work/${name}_s.png" >/dev/null 2>&1
            ;;
        wait:*)
            sleep "$(echo "${step#wait:}" | awk '{print $1/1000}')"
            ;;
        *)
            echo "$step" >> "$work/steps"
            sleep 0.2
            ;;
        esac
    done
    grep -a "FATAL\|Exception\|UnsatisfiedLink\|No implementation\|a2i.Act\|ActivityThread" "$work/vm.log" \
        | grep -av "unimplemented" | cut -c1-260 | tail -"${TAIL:-6}"
    ;;
stop)
    echo quit >> "$work/steps"
    sleep 1
    pkill -9 -x a2i-vm 2>/dev/null
    tail -2 "$work/vm.log" | cut -c1-200
    ;;
*)
    echo "usage: session.sh start <apk> | do <step>... | stop" >&2
    exit 2
    ;;
esac
