#!/bin/sh
# Builds the Android-ABI test library, converts it to a dylib, and runs it in the booted
# simulator. Needs the runtime and the packer to be built.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/out/tests/native"
frameworks="$root/out/runtime/iphonesimulator/Frameworks"
lld=$(find "$(rustc --print sysroot)" -name rust-lld | head -1)
mkdir -p "$out"

# An Android library, as the NDK would build it: Android's ABI, position independent,
# 16 KiB pages, stack protectors.
xcrun clang -target aarch64-linux-android24 -fPIC -O1 -fstack-protector-all -ffreestanding -fno-builtin \
    -c "$root/tests/native/android_abi_test.c" -o "$out/android_abi_test.o"
"$lld" -flavor gnu -shared -z max-page-size=16384 -z now --hash-style=both \
    -soname libandroid_abi_test.so -o "$out/libandroid_abi_test.so" "$out/android_abi_test.o"

"$root/target/release/philae" convert-so "$out/libandroid_abi_test.so" -o "$out/libandroid_abi_test.dylib" \
    --bionic "$frameworks/liba2i_bionic.dylib"
codesign --force --sign - "$out/libandroid_abi_test.dylib" 2>/dev/null

xcrun --sdk iphonesimulator clang -target arm64-apple-ios17.4-simulator -O1 -Wall \
    -Wl,-rpath,"$frameworks" -o "$out/harness" "$root/tests/native/harness.c"
xcrun simctl spawn booted "$out/harness" "$out/libandroid_abi_test.dylib" "/tmp/a2i-native-test-$$"

# The same for JNI: an Android library with native methods, loaded by the runtime.
jni_headers="$root/third_party/aosp/libnativehelper/include_jni"
xcrun clang -target aarch64-linux-android24 -fPIC -O1 -fstack-protector-all -ffreestanding -fno-builtin \
    -I"$jni_headers" -c "$root/tests/native/android_jni_test.c" -o "$out/android_jni_test.o"
"$lld" -flavor gnu -shared -z max-page-size=16384 -z now --hash-style=both \
    -soname libandroid_jni_test.so -o "$out/libandroid_jni_test.so" "$out/android_jni_test.o"
"$root/target/release/philae" convert-so "$out/libandroid_jni_test.so" -o "$out/libandroid_jni_test.dylib" \
    --bionic "$frameworks/liba2i_bionic.dylib"
codesign --force --sign - "$out/libandroid_jni_test.dylib" 2>/dev/null

rm -rf "$out/classes" && mkdir -p "$out/classes"
javac --release 17 -Xlint:-options -nowarn -d "$out/classes" "$root/tests/native/a2i/nativetest/NativeTest.java"
java_home=$(java -XshowSettings:properties -version 2>&1 | awk '/java.home/ {print $3}')
java -cp "$root/third_party/tools/r8.jar" com.android.tools.r8.D8 --min-api 35 --lib "$java_home" \
    --output "$out/nativetest.jar" "$out/classes/a2i/nativetest/NativeTest.class"
mkdir -p "/tmp/a2i-native-test-data-$$"
xcrun simctl spawn booted "$root/out/runtime/iphonesimulator/a2i-vm" "$root/out/runtime/android-root" \
    "/tmp/a2i-native-test-data-$$" -cp "$out/nativetest.jar" a2i.nativetest.NativeTest \
    "$out/libandroid_jni_test.dylib" 2>&1 | grep -v "setting boot class\|heap window\|Using Collector\|\.dm'"
