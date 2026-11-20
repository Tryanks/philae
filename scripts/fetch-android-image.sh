#!/bin/sh
# Assemble the Android root the runtime boots from: the boot class path jars (dex),
# ICU data and time zone data of Android 15, taken from Google's official emulator
# system image. Output: out/runtime/android-root/apex, laid out like a device's /apex.
#
# Needs 7-Zip (`7zz`, e.g. `brew install sevenzip`) to read the ext4 images.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/third_party/sysimg"
out="$root/out/runtime/android-root"
image=arm64-v8a-35_r02.zip
url=https://dl.google.com/android/repository/sys-img/android/$image

command -v 7zz >/dev/null || { echo "7zz not found (brew install sevenzip)"; exit 1; }
mkdir -p "$work"
cd "$work"

[ -f "$image" ] || { echo "download $image (733 MB)"; curl -fL --retry 3 -o "$image" "$url"; }
if [ ! -f system.part ]; then
    unzip -o -q "$image" arm64-v8a/system.img
    python3 -I "$root/scripts/lpunpack.py" arm64-v8a/system.img system system.part
    rm -f arm64-v8a/system.img
fi

# Each module is an APEX: a zip holding an ext4 image (compressed ones wrap that once more).
rm -rf apex && mkdir apex
for module in art conscrypt i18n tzdata; do
    name=com.android.$module
    7zz e -y -oapex system.part "system/apex/$name.apex" "system/apex/$name.capex" >/dev/null 2>&1 || true
    if [ -f "apex/$name.capex" ]; then
        unzip -o -q "apex/$name.capex" original_apex -d apex
        mv apex/original_apex "apex/$name.apex"
    fi
    unzip -o -q "apex/$name.apex" apex_payload.img -d "apex/$module"
    7zz x -y "-oapex/$module/root" "apex/$module/apex_payload.img" >/dev/null
done

rm -rf "$out/apex"
copy() {  # copy <module> <path inside the module>...
    module=$1; shift
    for path in "$@"; do
        mkdir -p "$out/apex/com.android.$module/$(dirname "$path")"
        cp -R "apex/$module/root/$path" "$out/apex/com.android.$module/$path"
    done
}
copy art javalib/core-oj.jar javalib/core-libart.jar javalib/okhttp.jar \
         javalib/bouncycastle.jar javalib/apache-xml.jar
copy conscrypt javalib/conscrypt.jar
# The trusted root certificates. Conscrypt reads them from its module's cacerts directory
# or, when that does not exist, from /system/etc/security/cacerts; which one it prefers
# depends on a version check, so they are provided in the place both paths end up at.
mkdir -p "$out/etc/security"
rm -rf "$out/etc/security/cacerts"
cp -R apex/conscrypt/root/cacerts "$out/etc/security/cacerts"
copy i18n javalib/core-icu4j.jar etc/icu
copy tzdata etc/tz
du -sh "$out"
