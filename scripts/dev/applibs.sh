#!/bin/sh
# Convert an APK's arm64 native libraries for the simulator, as the packer does when it
# builds an IPA. Prints the directory holding the converted libraries, for A2I_APP_LIBS.
#
#   applibs.sh <apk>
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
work=${A2I_WORK:-$root/out/dev}
apk=$1
dir=$work/applibs/$(basename "$apk" .apk)

rm -rf "$dir" && mkdir -p "$dir/so"
unzip -o -q -j "$apk" 'lib/arm64-v8a/*' -d "$dir/so" 2>/dev/null || true  # an APK may have none
siblings=""
for so in "$dir"/so/*.so; do
    [ -f "$so" ] && siblings="$siblings --sibling $so"
done
for so in "$dir"/so/*.so; do
    [ -f "$so" ] || continue
    name=$(basename "$so" .so)
    # shellcheck disable=SC2086  # $siblings is a list of arguments
    "$root/target/release/philae" convert-so "$so" -o "$dir/$name.dylib" \
        --bionic "$root/out/runtime/iphonesimulator/Frameworks/liba2i_bionic.dylib" $siblings 2>&1 \
        | grep -v "→" >&2 || true
    codesign --force --sign - "$dir/$name.dylib" 2>/dev/null
done
echo "$dir"
