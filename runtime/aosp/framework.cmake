# libandroid_framework: the framework's native code that this project builds itself.
#
# AOSP's host build of libandroid_runtime covers graphics, text and input event classes.
# What it leaves out on Darwin is built here from the same AOSP sources (the resource
# system) or implemented for iOS in runtime/framework.

set(CORE ${AOSP}/core)
set(FWB ${AOSP}/frameworks_base)

# --- libutils (the parts the resource system uses) --------------------------------------
aosp_library(aosp_utils core/libutils
    FileMap.cpp JenkinsHash.cpp LightRefBase.cpp misc.cpp Timers.cpp Tokenizer.cpp
    binder/Errors.cpp binder/RefBase.cpp binder/SharedBuffer.cpp binder/String16.cpp
    binder/String8.cpp binder/StrongPointer.cpp binder/Unicode.cpp binder/VectorImpl.cpp)
target_include_directories(aosp_utils PUBLIC
    ${CORE}/libutils/include ${CORE}/libutils/binder/include
    ${CORE}/libcutils/include ${CORE}/libsystem/include ${CORE}/libprocessgroup/include)
# libcutils' tracing entry points in their no-op host form: there is no atrace on iOS.
target_sources(aosp_utils PRIVATE ${CORE}/libcutils/trace-host.cpp)
target_compile_definitions(aosp_utils PUBLIC A2I_HAVE_UTILS_COMPAT)
target_compile_options(aosp_utils PRIVATE -fvisibility=hidden)
target_link_libraries(aosp_utils PUBLIC art)

# --- libandroidfw: resource tables, APK assets, binary XML -------------------------------
aosp_library(aosp_androidfw frameworks_base/libs/androidfw
    ApkAssets.cpp ApkParsing.cpp Asset.cpp AssetDir.cpp AssetManager.cpp AssetManager2.cpp
    AssetsProvider.cpp AttributeResolution.cpp BigBuffer.cpp BigBufferStream.cpp
    ChunkIterator.cpp ConfigDescription.cpp FileStream.cpp Idmap.cpp LoadedArsc.cpp Locale.cpp
    LocaleData.cpp misc.cpp NinePatch.cpp ObbFile.cpp PathUtils.cpp PosixUtils.cpp
    ResourceTimer.cpp ResourceTypes.cpp ResourceUtils.cpp StreamingZipInflater.cpp
    StringPool.cpp TypeWrappers.cpp Util.cpp ZipFileRO.cpp ZipUtils.cpp)
# libincfs-utils: the mapped-file wrapper the resource system reads everything through.
target_sources(aosp_androidfw PRIVATE ${AOSP}/incremental_delivery/incfs/util/map_ptr.cpp)
target_include_directories(aosp_androidfw PUBLIC
    ${FWB}/libs/androidfw/include ${FWB}/libs/androidfw/include_pathutils
    ${AOSP}/incremental_delivery/incfs/util/include
    ${AOSP}/frameworks_native/include
    ${AOSP}/libziparchive/include ${AOSP}/libziparchive/incfs_support/include)
target_compile_definitions(aosp_androidfw PUBLIC STATIC_ANDROIDFW_FOR_TOOLS)
target_compile_options(aosp_androidfw PRIVATE -fvisibility=hidden -std=gnu++2b)
target_link_libraries(aosp_androidfw PUBLIC aosp_utils art z a2i_linux_runtime)

# --- the library ------------------------------------------------------------------------
a2i_dylib(android_framework
    ${CMAKE_SOURCE_DIR}/framework/android_runtime.cpp
    ${CMAKE_SOURCE_DIR}/framework/android_media_AudioSystem.cpp
    ${CMAKE_SOURCE_DIR}/framework/android_os_Binder.cpp
    ${CMAKE_SOURCE_DIR}/framework/android_os_MessageQueue.cpp
    ${CMAKE_SOURCE_DIR}/framework/android_os_Parcel.cpp
    ${CMAKE_SOURCE_DIR}/framework/android_os_SystemClock.cpp
    ${CMAKE_SOURCE_DIR}/framework/android_os_SystemProperties.cpp
    ${CMAKE_SOURCE_DIR}/framework/android_view_DisplayEventReceiver.cpp
    ${CMAKE_SOURCE_DIR}/framework/misc_natives.cpp
    ${CMAKE_SOURCE_DIR}/framework/egl_natives.cpp
    ${FWB}/core/jni/android_util_AssetManager.cpp
    ${FWB}/core/jni/android_content_res_ApkAssets.cpp
    ${FWB}/core/jni/android_util_StringBlock.cpp
    ${FWB}/core/jni/android_util_XmlBlock.cpp)
# framework/include comes first: it replaces AOSP's AndroidRuntime.h and Binder helpers.
target_include_directories(android_framework BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/framework/include ${FWB}/core/jni ${FWB}/core/jni/include)
# For the Linux-named headers (<linux/capability.h>) that AssetManager's JNI includes,
# and libbinder's headers, which AOSP's android_util_Binder.h pulls in for its types.
target_include_directories(android_framework PRIVATE
    ${CMAKE_SOURCE_DIR}/compat/include ${AOSP}/frameworks_native/libs/binder/include)
# The runtime is ART, so @CriticalNative methods use ART's argument convention.
target_compile_definitions(android_framework PRIVATE A2I_ART_CRITICAL_NATIVES)
# (core_jni_helpers.h uses std::string without including it.)
target_compile_options(android_framework PRIVATE -std=gnu++2b "SHELL:-include string")
target_link_libraries(android_framework PRIVATE aosp_androidfw aosp_utils art a2i_linux_runtime)

# libstats_jni is loaded by name from android.util.StatsLog's static initialiser.
a2i_dylib(stats_jni ${CMAKE_SOURCE_DIR}/framework/stats_jni.cpp)
target_link_libraries(stats_jni PRIVATE art)

# Likewise libframework-connectivity-tiramisu-jni, from android.net.TrafficStats.
a2i_dylib(connectivity_jni ${CMAKE_SOURCE_DIR}/framework/connectivity_jni.cpp)
set_target_properties(connectivity_jni PROPERTIES OUTPUT_NAME framework-connectivity-tiramisu-jni)
target_link_libraries(connectivity_jni PRIVATE art)

# libmedia_jni is loaded by name from android.media.MediaPlayer's static initialiser.
a2i_dylib(media_jni ${CMAKE_SOURCE_DIR}/framework/media_jni.mm)
target_compile_options(media_jni PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(media_jni PRIVATE art "-framework AVFoundation" "-framework Foundation")

# libbionic: the C library and NDK functions that an app's own native libraries import,
# with Android's calling conventions and data layouts (see runtime/bionic).
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bionic_passthrough.S
    COMMAND python3 ${CMAKE_SOURCE_DIR}/bionic/gen_passthrough.py
            ${CMAKE_SOURCE_DIR}/bionic/passthrough.txt ${CMAKE_CURRENT_BINARY_DIR}/bionic_passthrough.S
    DEPENDS ${CMAKE_SOURCE_DIR}/bionic/gen_passthrough.py ${CMAKE_SOURCE_DIR}/bionic/passthrough.txt)
add_library(a2i_bionic SHARED
    ${CMAKE_SOURCE_DIR}/bionic/errno.c
    ${CMAKE_SOURCE_DIR}/bionic/stdio.c
    ${CMAKE_SOURCE_DIR}/bionic/fs.c
    ${CMAKE_SOURCE_DIR}/bionic/pthread.c
    ${CMAKE_SOURCE_DIR}/bionic/misc.c
    ${CMAKE_SOURCE_DIR}/bionic/ndk.c
    ${CMAKE_SOURCE_DIR}/bionic/variadic.S
    ${CMAKE_CURRENT_BINARY_DIR}/bionic_passthrough.S)
set_target_properties(a2i_bionic PROPERTIES
    MACOSX_RPATH ON BUILD_WITH_INSTALL_NAME_DIR ON INSTALL_NAME_DIR "@rpath"
    C_VISIBILITY_PRESET hidden)
target_compile_options(a2i_bionic PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
target_link_libraries(a2i_bionic PRIVATE z)
