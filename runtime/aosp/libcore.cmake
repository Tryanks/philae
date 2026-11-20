# The runtime's shared libraries, mirroring the .so files of Android's ART and i18n modules:
#
#   libart        the runtime and everything linked into it on Android (libartbase, libdexfile,
#                 libbase, liblog, libnativehelper, libopenjdkjvm, libandroidio, ...)
#   libicu        ICU4C (libicuuc + libicui18n) and the code that locates its data files
#   libicu_jni    natives of the android.icu / com.android.icu classes
#   libjavacore   natives of libcore's luni classes (libcore.io.Linux, ...)
#   libopenjdk    natives of the OpenJDK-derived classes (java.io, java.net, java.nio, ...)
#
# The last three are loaded by name at runtime start (Runtime::InitNativeMethods), which is
# why they are separate libraries: each has its own JNI_OnLoad and its own private helpers.

function(a2i_dylib name)
    add_library(${name} SHARED ${ARGN})
    set_target_properties(${name} PROPERTIES
        MACOSX_RPATH ON
        BUILD_WITH_INSTALL_NAME_DIR ON
        INSTALL_NAME_DIR "@rpath")
    target_link_libraries(${name} PRIVATE aosp_flags)
    target_link_options(${name} PRIVATE -Wl,-no_warn_duplicate_libraries)
endfunction()

# Compile settings for libcore's native code, which is written against Linux/bionic.
# Two ways to use it: a2i_linux_runtime only links the behaviour (Android path relocation
# in particular), for code that already builds on Darwin; a2i_linux_compat also force-
# includes the header of Linux spellings, for code written against bionic alone.
add_library(a2i_linux_runtime STATIC ${CMAKE_SOURCE_DIR}/compat/linux_compat.c)
target_include_directories(a2i_linux_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/include)
target_compile_options(a2i_linux_runtime PRIVATE -Wall -Wextra -fvisibility=hidden)

add_library(a2i_linux_compat INTERFACE)
target_include_directories(a2i_linux_compat INTERFACE ${CMAKE_SOURCE_DIR}/compat/include)
target_compile_options(a2i_linux_compat INTERFACE
    "SHELL:-include ${CMAKE_SOURCE_DIR}/compat/include/a2i_linux_compat.h")
target_link_libraries(a2i_linux_compat INTERFACE a2i_linux_runtime)

# --- libart ---------------------------------------------------------------------------
set(NATIVEHELPER ${AOSP}/libnativehelper)
add_library(aosp_jni_headers INTERFACE)
target_include_directories(aosp_jni_headers INTERFACE
    ${NATIVEHELPER}/include_jni
    ${NATIVEHELPER}/include
    ${NATIVEHELPER}/header_only_include
    ${NATIVEHELPER}/include_platform
    ${NATIVEHELPER}/include_platform_header_only
    ${AOSP}/libbase/include
    ${AOSP}/fmtlib/include
    ${AOSP}/logging/liblog/include
    ${AOSP}/libziparchive/include)
# libbase's headers use fmt; see aosp_base for these settings.
target_compile_definitions(aosp_jni_headers INTERFACE FMT_HEADER_ONLY=1 FMT_CONSTEVAL=)

a2i_dylib(art
    # libnativehelper, minus JniInvocation: that is the indirection through which Android
    # finds libart.so at run time, and here JNI_CreateJavaVM is ART's own.
    ${NATIVEHELPER}/ExpandableString.c
    ${NATIVEHELPER}/JNIHelp.c
    ${NATIVEHELPER}/JNIPlatformHelp.c
    ${NATIVEHELPER}/JniConstants.c
    ${NATIVEHELPER}/file_descriptor_jni.c
    # libopenjdkjvm: the JVM_* entry points libopenjdk calls back into.
    ${AOSP}/art/openjdkjvm/OpenjdkJvm.cc
    # libandroidio
    ${AOSP}/libcore/luni/src/main/native/AsynchronousCloseMonitor.cpp)
target_include_directories(art PRIVATE ${NATIVEHELPER} ${AOSP}/libcore/luni/src/main/native)
target_link_libraries(art
    PUBLIC aosp_jni_headers
    PRIVATE art_runtime_headers
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,art_runtime,art_dexfile,art_base,art_profile,art_elffile>"
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,art_palette,art_sigchain,art_nativebridge,art_nativeloader>"
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,aosp_ziparchive,aosp_base,aosp_log>"
            aosp_lz4 aosp_lzma aosp_cpu_features z
            # Android paths reach the runtime too (class paths, zip files): relocate them.
            a2i_linux_runtime)

# --- libicu ---------------------------------------------------------------------------
set(ICU ${AOSP}/icu)
file(GLOB ICU_COMMON_SRCS ${ICU}/icu4c/source/common/*.cpp)
file(GLOB ICU_I18N_SRCS ${ICU}/icu4c/source/i18n/*.cpp)
a2i_dylib(icu
    ${ICU_COMMON_SRCS} ${ICU_I18N_SRCS}
    ${ICU}/icu4c/source/stubdata/stubdata.cpp
    ${ICU}/libandroidicuinit/IcuRegistration.cpp
    ${ICU}/libandroidicuinit/android_icu_init.cpp)
target_include_directories(icu
    PUBLIC ${ICU}/icu4c/source/common ${ICU}/icu4c/source/i18n
    PRIVATE ${ICU}/libandroidicuinit/include ${ICU}/libandroidicuinit)
# libicuuc and libicui18n in one library: each half still has to be compiled as itself,
# because that is what makes its own classes exported.
set_source_files_properties(${ICU_COMMON_SRCS} PROPERTIES COMPILE_DEFINITIONS U_COMMON_IMPLEMENTATION)
set_source_files_properties(${ICU_I18N_SRCS} PROPERTIES COMPILE_DEFINITIONS U_I18N_IMPLEMENTATION)
target_compile_definitions(icu
    PRIVATE _REENTRANT
    PUBLIC UCONFIG_USE_ML_PHRASE_BREAKING=1)
# ICU uses RTTI and exports through its own U_EXPORT annotations.
target_compile_options(icu PRIVATE -frtti -fvisibility=hidden)
target_link_libraries(icu PRIVATE art)

# --- libicu_jni -----------------------------------------------------------------------
file(GLOB ICU_JNI_SRCS ${ICU}/android_icu4j/libcore_bridge/src/native/*.cpp)
a2i_dylib(icu_jni ${ICU_JNI_SRCS})
target_compile_definitions(icu_jni PRIVATE U_USING_ICU_NAMESPACE=0)
target_link_libraries(icu_jni PRIVATE icu art a2i_linux_compat)

# --- BoringSSL and libjavacrypto ------------------------------------------------------
# AOSP's BoringSSL ships its source lists as CMake variables. The assembly files cover
# every platform and guard themselves, so all of them are compiled.
set(BORINGSSL_ROOT ${AOSP}/boringssl/)
include(${AOSP}/boringssl/android-sources.cmake)
add_library(aosp_crypto STATIC ${crypto_sources} ${crypto_sources_asm})
target_include_directories(aosp_crypto PUBLIC ${AOSP}/boringssl/src/include)
target_compile_definitions(aosp_crypto PRIVATE BORINGSSL_IMPLEMENTATION OPENSSL_SMALL)
target_compile_options(aosp_crypto PRIVATE -w -fvisibility=hidden)
add_library(aosp_ssl STATIC ${ssl_sources})
target_compile_definitions(aosp_ssl PRIVATE BORINGSSL_IMPLEMENTATION OPENSSL_SMALL)
target_compile_options(aosp_ssl PRIVATE -w -fvisibility=hidden -fno-exceptions -fno-rtti)
target_link_libraries(aosp_ssl PUBLIC aosp_crypto)

# Conscrypt's natives: the Java security provider (hashes, ciphers, TLS) over BoringSSL.
file(GLOB CONSCRYPT_SRCS ${AOSP}/conscrypt/common/src/jni/main/cpp/conscrypt/*.cc)
a2i_dylib(javacrypto ${CONSCRYPT_SRCS})
target_include_directories(javacrypto PRIVATE ${AOSP}/conscrypt/common/src/jni/main/include)
# ANDROID: build Conscrypt as the platform's bundled provider (Android's jni.h and
# logging, libandroidio for interruptible I/O), not as the standalone OpenJDK library.
target_include_directories(javacrypto PRIVATE ${AOSP}/libcore/luni/src/main/native ${AOSP}/libcore/include)
target_compile_definitions(javacrypto PRIVATE JNI_JARJAR_PREFIX=com/android/ ANDROID)
target_compile_options(javacrypto PRIVATE -fvisibility=hidden)
target_link_libraries(javacrypto PRIVATE aosp_ssl aosp_crypto art a2i_linux_compat)

# --- libjavacore ----------------------------------------------------------------------
aosp_library(aosp_expat expat/expat/lib xmlparse.c xmlrole.c xmltok.c)
target_include_directories(aosp_expat PUBLIC ${AOSP}/expat/expat/lib PRIVATE ${AOSP}/expat)
target_compile_definitions(aosp_expat PRIVATE HAVE_EXPAT_CONFIG_H)
target_compile_options(aosp_expat PRIVATE -fno-strict-aliasing -include stdio.h -fvisibility=hidden)

set(LUNI ${AOSP}/libcore/luni/src/main/native)
a2i_dylib(javacore
    ${LUNI}/ExecStrings.cpp ${LUNI}/IcuUtilities.cpp ${LUNI}/JniConstants.cpp
    ${LUNI}/JniException.cpp ${LUNI}/NetworkUtilities.cpp ${LUNI}/Register.cpp
    ${LUNI}/ZipUtilities.cpp ${LUNI}/android_system_OsConstants.cpp ${LUNI}/cbigint.cpp
    ${LUNI}/java_lang_StringToReal.cpp ${LUNI}/java_lang_invoke_MethodHandle.cpp
    ${LUNI}/java_lang_invoke_VarHandle.cpp ${LUNI}/libcore_math_NativeBN.cpp
    ${LUNI}/libcore_icu_ICU.cpp ${LUNI}/libcore_io_AsynchronousCloseMonitor.cpp
    ${LUNI}/libcore_io_Linux.cpp ${LUNI}/libcore_io_Memory.cpp
    ${LUNI}/libcore_util_NativeAllocationRegistry.cpp
    ${LUNI}/org_apache_harmony_xml_ExpatParser.cpp ${LUNI}/sun_misc_Unsafe.cpp
    ${LUNI}/valueOf.cpp)
target_include_directories(javacore PRIVATE ${AOSP}/libcore/include)
# ANDROID_HOST_MUSL: "this libc's strerror_r is the POSIX one", true of Darwin as well.
target_compile_definitions(javacore PRIVATE U_USING_ICU_NAMESPACE=0 ANDROID_HOST_MUSL)
target_link_libraries(javacore PRIVATE icu art aosp_expat aosp_crypto z a2i_linux_compat)

# --- libopenjdk -----------------------------------------------------------------------
file(GLOB FDLIBM_SRCS ${AOSP}/fdlibm/k_*.c ${AOSP}/fdlibm/e_*.c ${AOSP}/fdlibm/w_*.c ${AOSP}/fdlibm/s_*.c)
add_library(aosp_fdlibm STATIC ${FDLIBM_SRCS})
target_include_directories(aosp_fdlibm PUBLIC ${AOSP}/fdlibm)
target_compile_definitions(aosp_fdlibm PRIVATE _IEEE_LIBM __LITTLE_ENDIAN)
target_compile_options(aosp_fdlibm PRIVATE -fno-strict-aliasing -fvisibility=hidden)
target_link_libraries(aosp_fdlibm PRIVATE aosp_flags)

set(OJLUNI ${AOSP}/libcore/ojluni/src/main/native)
# Not built yet: EPoll.c and EPollPort.c (asynchronous channels, need epoll) and
# LinuxWatchService.c (java.nio.file.WatchService, needs inotify).
set(OPENJDK_SRCS
    ZipFile.c Inflater.c Deflater.c CRC32.c Adler32.c zip_util.c jni_util.c jni_util_md.c
    io_util.c canonicalize_md.c FileDescriptor_md.c DatagramChannelImpl.c DatagramDispatcher.c
    Console_md.c IOUtil.c PollArrayWrapper.c SocketChannelImpl.c FileChannelImpl.c
    FileDispatcherImpl.c FileInputStream.c FileSystemPreferences.c
    UnixAsynchronousServerSocketChannelImpl.c UnixAsynchronousSocketChannelImpl.c io_util_md.c
    NativeThread.c FileKey.c UnixFileSystem_md.c ObjectStreamClass.c ObjectOutputStream.c
    ObjectInputStream.c LinuxNativeDispatcher.c UnixCopyFile.c
    UnixNativeDispatcher.c InetAddress.c net_util.c net_util_md.c Net.c MappedByteBuffer.c
    Inet6Address.c Inet4Address.c linux_close.cpp ServerSocketChannelImpl.c SocketInputStream.c
    SocketOutputStream.c Float.c Double.c StrictMath.c Math.c ProcessEnvironment_md.c System.c
    Runtime.c UNIXProcess_md.c Character.cpp socket_tagger_util.cpp OnLoad.cpp JniConstants.cpp
    UnixDomainSockets.c jdk_internal_misc_VM.cpp)
list(TRANSFORM OPENJDK_SRCS PREPEND ${OJLUNI}/)
a2i_dylib(openjdk ${OPENJDK_SRCS})
# StrictMath.c reaches fdlibm through a path relative to the root of an AOSP checkout.
file(MAKE_DIRECTORY ${ART_GEN}/aosp_root/x/y)
file(CREATE_LINK ${AOSP} ${ART_GEN}/aosp_root/external SYMBOLIC)
target_include_directories(openjdk PRIVATE ${OJLUNI} ${LUNI} ${AOSP}/libcore/include ${ART_GEN}/aosp_root/x/y)
# The OpenJDK sources keep their BSD/macOS branches; select them.
target_compile_definitions(openjdk PRIVATE _ALLBSD_SOURCE MACOSX)
# (CoreFoundation: the macOS branch normalises file names to NFC with CFString.)
target_link_libraries(openjdk PRIVATE icu art aosp_fdlibm aosp_crypto z a2i_linux_compat "-framework CoreFoundation")
