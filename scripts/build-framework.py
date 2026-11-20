#!/usr/bin/env python3
"""Build the Android framework for the runtime from Robolectric's android-all jar.

android-all is AOSP's framework compiled as ordinary class files, published together
with the host build of its native code (libandroid_runtime) that the simulator runtime
uses. This script turns it into what the runtime boots from:

  out/runtime/android-root/framework/framework.jar       the classes, as dex
  out/runtime/android-root/framework/framework-res.apk   the platform resources
  out/runtime/android-root/usr/share/zoneinfo            time zone data it expects
  out/runtime/android-root/fonts, etc/fonts.xml          system fonts (from Robolectric's
                                                         nativeruntime-dist-compat jar)

Classes that the ART module jars (core-oj, core-libart, ...) already define are left
out: those jars come first on the boot class path and must stay the single definition.
"""
import os
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ANDROID_ALL = os.path.join(ROOT, "third_party/robolectric/android-all-15.jar")
ANDROID_ROOT = os.path.join(ROOT, "out/runtime/android-root")
NATIVE_RUNTIME_DIST = os.path.join(ROOT, "third_party/robolectric/nrt.jar")
R8 = os.path.join(ROOT, "third_party/tools/r8.jar")
WORK = os.path.join(ROOT, "out/framework")


def dex_class_names(dex):
    """Internal names (a/b/C) of the classes a dex file defines."""
    string_ids_off, = struct.unpack_from("<I", dex, 0x3C)
    type_ids_off, = struct.unpack_from("<I", dex, 0x44)
    class_defs_size, class_defs_off = struct.unpack_from("<II", dex, 0x60)
    for i in range(class_defs_size):
        type_idx, = struct.unpack_from("<I", dex, class_defs_off + 32 * i)
        string_idx, = struct.unpack_from("<I", dex, type_ids_off + 4 * type_idx)
        off, = struct.unpack_from("<I", dex, string_ids_off + 4 * string_idx)
        while dex[off] & 0x80:  # skip the ULEB128 length
            off += 1
        off += 1
        end = dex.index(b"\0", off)
        yield dex[off + 1:end - 1].decode()  # strip "L" and ";"


def module_classes():
    """Every class defined by the ART/i18n/conscrypt module jars."""
    names = set()
    for dirpath, _, files in os.walk(os.path.join(ANDROID_ROOT, "apex")):
        for name in files:
            if not name.endswith(".jar"):
                continue
            with zipfile.ZipFile(os.path.join(dirpath, name)) as jar:
                for entry in jar.namelist():
                    if entry.startswith("classes") and entry.endswith(".dex"):
                        names.update(dex_class_names(jar.read(entry)))
    return names


def install_fonts():
    """System fonts and their configuration, where the framework looks for them."""
    fonts_dir = os.path.join(ANDROID_ROOT, "fonts")
    os.makedirs(fonts_dir, exist_ok=True)
    os.makedirs(os.path.join(ANDROID_ROOT, "etc"), exist_ok=True)
    count = 0
    with zipfile.ZipFile(NATIVE_RUNTIME_DIST) as dist:
        for name in dist.namelist():
            if not name.startswith("fonts/") or name.endswith("/"):
                continue
            base = os.path.basename(name)
            # The CJK families come in seven weights of ~18 MB each; Regular is enough
            # to display text, and missing files are skipped by the font loader.
            if base.startswith(("NotoSansCJK-", "NotoSerifCJK-")) and "Regular" not in base:
                continue
            target = os.path.join(ANDROID_ROOT, "etc", base) if base.endswith(".xml") \
                else os.path.join(fonts_dir, base)
            if base.endswith((".ttf", ".ttc", ".otf", ".xml")):
                with open(target, "wb") as f:
                    f.write(dist.read(name))
                count += 1
    # Android 15 reads its font configuration from font_fallback.xml; the parser accepts
    # the older fonts.xml content under that name.
    etc = os.path.join(ANDROID_ROOT, "etc")
    with open(os.path.join(etc, "fonts.xml"), "rb") as src, \
            open(os.path.join(etc, "font_fallback.xml"), "wb") as dst:
        dst.write(src.read())
    print(f"fonts: {count} files")


def main():
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(os.path.join(ANDROID_ROOT, "framework"), exist_ok=True)
    provided = module_classes()
    if not provided:
        sys.exit("no module jars found; run scripts/fetch-android-image.sh first")

    classes_jar = os.path.join(WORK, "framework-classes.jar")
    res_apk = os.path.join(ANDROID_ROOT, "framework/framework-res.apk")
    kept = skipped = 0
    with zipfile.ZipFile(ANDROID_ALL) as src, \
            zipfile.ZipFile(classes_jar, "w", zipfile.ZIP_STORED) as classes, \
            zipfile.ZipFile(res_apk, "w", zipfile.ZIP_DEFLATED) as res:
        for info in src.infolist():
            name = info.filename
            if name.endswith("/"):
                continue
            if name.endswith(".class"):
                if name[:-len(".class")] in provided:
                    skipped += 1
                else:
                    classes.writestr(info, src.read(name))
                    kept += 1
            elif name.startswith(("res/", "assets/")) or name in ("resources.arsc", "AndroidManifest.xml"):
                # resources.arsc must be stored uncompressed: the asset manager maps it.
                method = zipfile.ZIP_STORED if name == "resources.arsc" else zipfile.ZIP_DEFLATED
                res.writestr(zipfile.ZipInfo(name, date_time=info.date_time), src.read(name), method)
            elif name.startswith("usr/"):
                target = os.path.join(ANDROID_ROOT, name)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(src.read(name))
    print(f"framework classes: {kept} kept, {skipped} already provided by module jars")
    install_fonts()

    # Desugaring stays on even at API level 35: the classes use Java 11 nest-based private
    # access, which dex cannot express and D8 turns into accessor methods. The JDK serves
    # as the class library D8 resolves java.* against.
    java_home = subprocess.run(["java", "-XshowSettings:properties", "-version"],
                               capture_output=True, text=True).stderr
    java_home = next(line.split("=", 1)[1].strip() for line in java_home.splitlines()
                     if "java.home" in line)
    out = os.path.join(ANDROID_ROOT, "framework/framework.jar")
    subprocess.run(
        ["java", "-Xmx8g", "-cp", R8, "com.android.tools.r8.D8", "--release", "--min-api", "35",
         "--lib", java_home, "--output", out, classes_jar],
        check=True)
    with zipfile.ZipFile(out) as jar:
        dex = [(i.filename, i.file_size) for i in jar.infolist()]
    print("framework.jar:", ", ".join(f"{n} {s >> 20} MB" for n, s in dex))


if __name__ == "__main__":
    main()
