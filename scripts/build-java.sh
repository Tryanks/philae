#!/bin/sh
# Compile the runtime's own Java code (runtime/java) to dex and put it, with the system
# properties file, in the Android root: out/runtime/android-root/framework/a2i-boot.jar.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
r8="$root/third_party/tools/r8.jar"
if [ ! -f "$r8" ]; then
    # D8, Android's dex compiler, from Google's Maven repository.
    mkdir -p "$(dirname "$r8")"
    curl -fL --retry 3 -o "$r8" https://dl.google.com/android/maven2/com/android/tools/r8/8.13.25/r8-8.13.25.jar
fi
# D8 resolves and desugars against a class library; the JDK's own is sufficient here.
java_home=$(java -XshowSettings:properties -version 2>&1 | awk '/java.home/ {print $3}')

# The code is written against the Android framework, hidden APIs included; Robolectric's
# android-all jar is that framework as class files.
framework="$root/third_party/robolectric/android-all-15.jar"
[ -f "$framework" ] || { echo "missing $framework"; exit 1; }

android_root="$root/out/runtime/android-root"
build="$root/out/java"
rm -rf "$build" && mkdir -p "$build/classes" "$android_root/framework"
find "$root/runtime/java" -name '*.java' > "$build/sources.txt"
javac --release 17 -Xlint:-options -nowarn -proc:none -cp "$framework" -d "$build/classes" @"$build/sources.txt"
find "$build/classes" -name '*.class' > "$build/classes.txt"
java -cp "$r8" com.android.tools.r8.D8 --min-api 35 --lib "$java_home" --lib "$framework" \
    --output "$android_root/framework/a2i-boot.jar" @"$build/classes.txt"
cp "$root/runtime/system/build.prop" "$android_root/build.prop"
