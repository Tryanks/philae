#!/bin/sh
# Fetch the pinned AOSP sources the runtime is built from into third_party/aosp/, and apply
# this project's patches to them. Shallow clones of one release tag keep this to roughly
# 1 GB instead of a full AOSP tree.
#
# Our changes to AOSP code are kept as one patch per repository. To update one after
# editing the checkout: git -C third_party/aosp/<name> diff > runtime/aosp/patches/<name>.patch
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tag=android-15.0.0_r1
dest="$root/third_party/aosp"
mkdir -p "$dest"

# fetch <directory name> <path under https://android.googlesource.com/platform/> [sparse paths...]
# Clones into a temporary directory that only takes its final name once it is complete and
# patched, so an interrupted run is simply repeated. The server sometimes stalls in the
# middle of a transfer; such a clone is abandoned and tried again.
fetch() {
    name=$1; path=$2; shift 2
    if [ -d "$dest/$name/.git" ]; then
        echo "have $name"
        return
    fi
    echo "fetch $name ($tag${1:+, sparse})"
    part="$dest/$name.part"
    attempt=1
    while :; do
        rm -rf "$part"
        if git -c advice.detachedHead=false -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=30 \
            clone --quiet --depth 1 --branch "$tag" ${1:+--filter=blob:none --sparse} \
            "https://android.googlesource.com/platform/$path" "$part" \
            && { [ $# -eq 0 ] || git -C "$part" sparse-checkout set "$@"; }; then
            break
        fi
        [ $attempt -lt 4 ] || { echo "could not fetch $name"; exit 1; }
        attempt=$((attempt + 1))
        echo "  again (attempt $attempt)"
    done
    patch="$root/runtime/aosp/patches/$name.patch"
    if [ -f "$patch" ]; then
        echo "patch $name"
        git -C "$part" apply "$patch"
    fi
    mv "$part" "$dest/$name"
}

while read -r name path; do
    [ -n "$name" ] || continue
    fetch "$name" "$path"
done <<LIST
art art
libbase system/libbase
logging system/logging
libziparchive system/libziparchive
fmtlib external/fmtlib
tinyxml2 external/tinyxml2
core system/core
libnativehelper libnativehelper
lz4 external/lz4
lzma external/lzma
cpu_features external/cpu_features
libcore libcore
fdlibm external/fdlibm
dlmalloc external/dlmalloc
musl external/musl
icu external/icu
expat external/expat
incremental_delivery system/incremental_delivery
boringssl external/boringssl
conscrypt external/conscrypt
LIST

# frameworks/base is several GB even as a shallow clone; only the framework's native
# resource system and JNI sources are needed, so fetch just those directories.
fetch frameworks_base frameworks/base libs/androidfw core/jni libs/hwui/apex
# Likewise frameworks/native, for the NDK's <android/*.h> headers.
fetch frameworks_native frameworks/native include/android libs/binder
# bionic, Android's C library: only its headers, which define the ABI (constants and
# structure layouts) that the runtime's bionic compatibility library reproduces.
fetch bionic bionic libc/include libc/kernel/uapi libc/kernel/android
