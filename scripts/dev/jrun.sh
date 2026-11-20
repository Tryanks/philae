#!/bin/sh
# Compile one scratch Java class against the framework and run its main() in the
# simulator's command-line VM. For trying a framework API in isolation.
#
#   jrun.sh <File.java> [seconds] [vm or program args...]
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${A2I_WORK:-$root/out/dev}
src=$1
shift
secs=${1:-60}
[ $# -gt 0 ] && shift
class=$(basename "$src" .java)
dir=$work/jrun
framework=$root/third_party/robolectric/android-all-15.jar

rm -rf "$dir" && mkdir -p "$dir/classes"
javac --release 17 -Xlint:-options -nowarn -proc:none -cp "$root/out/java/classes:$framework" \
    -d "$dir/classes" "$src" 2>&1 | grep -v "^Note:" | head -30
[ -f "$dir/classes/$class.class" ] || { echo "COMPILE FAILED"; exit 1; }
java_home=$(java -XshowSettings:properties -version 2>&1 | awk '/java.home/ {print $3}')
find "$dir/classes" -name '*.class' > "$dir/classes.txt"
java -cp "$root/third_party/tools/r8.jar" com.android.tools.r8.D8 --min-api 35 --lib "$java_home" \
    --lib "$framework" --output "$dir/t.jar" @"$dir/classes.txt" 2>&1 | grep -v "^Warning\|^  " | head -5
"$here/runvm.sh" "$secs" -cp "$dir/t.jar" "$@" "$class"
grep -av "setting boot class\|Using Collector\|\.dm'" "$work/vm.log" | cut -c1-400 | head -"${LINES_MAX:-60}"
