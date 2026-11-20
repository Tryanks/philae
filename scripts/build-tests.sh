#!/bin/sh
# Compile the Java tests under tests/java into dex: out/tests/smoke.jar (classes.dex inside).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
r8="$root/third_party/tools/r8.jar"
[ -f "$r8" ] || { echo "missing $r8 (run scripts/build-java.sh first)"; exit 1; }
out="$root/out/tests"
rm -rf "$out/classes" && mkdir -p "$out/classes"
find "$root/tests/java" -name '*.java' > "$out/sources.txt"
javac --release 11 -Xlint:-options -d "$out/classes" @"$out/sources.txt"
find "$out/classes" -name '*.class' > "$out/classes.txt"
# D8 desugars against a class library; the JDK's own is sufficient for these tests.
java_home=$(java -XshowSettings:properties -version 2>&1 | awk '/java.home/ {print $3}')
java -cp "$r8" com.android.tools.r8.D8 --min-api 26 --lib "$java_home" \
    --output "$out/smoke.jar" @"$out/classes.txt"
echo "$out/smoke.jar"
