#!/bin/sh
# Run the command-line VM in the booted simulator, with a time limit.
#
#   runvm.sh [seconds] [vm args...]
#
# The VM runs with the simulator's working directory: give it absolute paths.
# Output goes to <work>/vm.log, where <work> is $A2I_WORK or out/dev. Prints the exit
# status, or TIMEOUT if the VM had to be killed.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
work=${A2I_WORK:-$root/out/dev}
mkdir -p "$work/data"
secs=${1:-20}
[ $# -gt 0 ] && shift

xcrun simctl spawn booted "$root/out/runtime/iphonesimulator/a2i-vm" \
    "$root/out/runtime/android-root" "$work/data" "$@" > "$work/vm.log" 2>&1 &
pid=$!
i=0
while [ "$i" -lt "$secs" ] && kill -0 $pid 2>/dev/null; do sleep 1; i=$((i + 1)); done
if kill -0 $pid 2>/dev/null; then
    echo "TIMEOUT after ${secs}s"
    pkill -9 -x a2i-vm
fi
wait $pid 2>/dev/null
echo "exit=$?"
