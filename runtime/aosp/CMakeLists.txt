# Static libraries built from the pinned AOSP sources in third_party/aosp
# (see scripts/fetch-aosp.sh). Source lists mirror the corresponding Android.bp
# host/darwin variants; only what the runtime links is built.

set(AOSP ${CMAKE_SOURCE_DIR}/../third_party/aosp)
if(NOT EXISTS ${AOSP}/art/libdexfile)
    message(FATAL_ERROR "AOSP sources missing; run scripts/fetch-aosp.sh")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# AOSP builds with its own warning policy; ours does not apply to vendored code.
add_library(aosp_flags INTERFACE)
# Android compiles the platform with zero-initialised locals, and AOSP code has come to rely
# on it (ART's vtable linker reads an alloca'd bit vector it never clears, for one).
target_compile_options(aosp_flags INTERFACE -w -fno-exceptions
    -ftrivial-auto-var-init=zero
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/a2i_darwin_compat.h")
target_include_directories(aosp_flags INTERFACE shim)

function(aosp_library name dir)
    list(TRANSFORM ARGN PREPEND ${AOSP}/${dir}/)
    add_library(${name} STATIC ${ARGN})
    target_link_libraries(${name} PRIVATE aosp_flags)
endfunction()

# --- liblog: host flavour, logs to stderr ---------------------------------------------
aosp_library(aosp_log logging/liblog
    log_event_list.cpp log_event_write.cpp logger_name.cpp logger_write.cpp properties.cpp)
target_include_directories(aosp_log
    PUBLIC ${AOSP}/logging/liblog/include
    PRIVATE ${AOSP}/core/libcutils/include)
target_compile_definitions(aosp_log PRIVATE LIBLOG_LOG_TAG=1006 SNET_EVENT_LOG_TAG=1397638484)
target_link_libraries(aosp_log PRIVATE aosp_base)

# --- libbase --------------------------------------------------------------------------
aosp_library(aosp_base libbase
    chrono_utils.cpp errors_unix.cpp file.cpp hex.cpp
    logging.cpp mapped_file.cpp parsebool.cpp parsenetaddress.cpp posix_strerror_r.cpp
    process.cpp properties.cpp result.cpp stringprintf.cpp strings.cpp threads.cpp)
target_include_directories(aosp_base PUBLIC ${AOSP}/libbase/include ${AOSP}/fmtlib/include)
# This fmt release predates current clang's stricter consteval checking.
target_compile_definitions(aosp_base PUBLIC FMT_HEADER_ONLY=1 FMT_CONSTEVAL=)
target_link_libraries(aosp_base PUBLIC aosp_log)

# --- libziparchive --------------------------------------------------------------------
aosp_library(aosp_ziparchive libziparchive
    zip_archive.cc zip_archive_stream_entry.cc zip_cd_entry_map.cc zip_error.cpp zip_writer.cc)
target_include_directories(aosp_ziparchive
    PUBLIC ${AOSP}/libziparchive/include
    PRIVATE ${AOSP}/libziparchive/incfs_support/include)
target_compile_definitions(aosp_ziparchive PRIVATE ZLIB_CONST INCFS_SUPPORT_DISABLED=1)
target_link_libraries(aosp_ziparchive PUBLIC aosp_base aosp_log z)

# --- ART ------------------------------------------------------------------------------
# Compile settings shared by every ART library (art_defaults + build/art.go, host flavour).
# ART_TARGET* stay undefined: to ART this is a non-Android host, configured through
# ANDROID_ROOT/ANDROID_DATA rather than fixed /system paths.
add_library(art_flags INTERFACE)
target_compile_options(art_flags INTERFACE -fno-rtti -fno-omit-frame-pointer)
target_compile_definitions(art_flags INTERFACE
    ART_PAGE_SIZE_AGNOSTIC=1      # iOS pages are 16 KiB
    ART_ENABLE_CODEGEN_arm64
    ART_STACK_OVERFLOW_GAP_arm=8192     # instruction_set.h wants a gap for every ISA
    ART_STACK_OVERFLOW_GAP_arm64=8192
    ART_STACK_OVERFLOW_GAP_riscv64=8192
    ART_STACK_OVERFLOW_GAP_x86=8192
    ART_STACK_OVERFLOW_GAP_x86_64=8192
    ART_FRAME_SIZE_LIMIT=1744
    ART_BASE_ADDRESS=0x70000000
    ART_BASE_ADDRESS_MIN_DELTA=-0x1000000
    ART_BASE_ADDRESS_MAX_DELTA=0x1000000
    # Mark-sweep, no read barrier: the default collector (CMC) needs Linux userfaultfd.
    ART_DEFAULT_GC_TYPE_IS_CMS
    ART_STATIC_LIBARTBASE
    USE_D8_DESUGAR=1)
target_link_libraries(art_flags INTERFACE aosp_flags)

# ART generates operator<< for its enums from the headers that declare them.
function(art_operator_out out_var lib)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/${lib}_operator_out.cc)
    list(TRANSFORM ARGN PREPEND art/${lib}/)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${Python3_EXECUTABLE} art/tools/generate_operator_out.py art/${lib} ${ARGN} > ${out}
        WORKING_DIRECTORY ${AOSP}
        DEPENDS ${AOSP}/art/tools/generate_operator_out.py
        VERBATIM)
    set(${out_var} ${out} PARENT_SCOPE)
endfunction()

aosp_library(art_palette art/libartpalette system/palette_fake.cc)
target_include_directories(art_palette PUBLIC
    ${AOSP}/art/libartpalette/include
    ${AOSP}/libnativehelper/include_jni)
target_link_libraries(art_palette PRIVATE art_flags aosp_base aosp_log)

art_operator_out(ARTBASE_OPERATOR_OUT libartbase
    arch/instruction_set.h base/allocator.h base/unix_file/fd_file.h)
aosp_library(art_base art/libartbase
    arch/instruction_set.cc
    base/allocator.cc base/arena_allocator.cc base/arena_bit_vector.cc base/bit_vector.cc
    base/compiler_filter.cc base/file_magic.cc base/file_utils.cc base/flags.cc
    base/globals_unix.cc base/hex_dump.cc base/logging.cc base/malloc_arena_pool.cc
    base/membarrier.cc base/memfd.cc base/memory_region.cc base/mem_map.cc
    base/mem_map_unix.cc base/metrics/metrics_common.cc base/os_linux.cc
    base/pointer_size.cc base/runtime_debug.cc base/scoped_arena_allocator.cc
    base/scoped_flock.cc base/socket_peer_is_trusted.cc base/time_utils.cc
    base/unix_file/fd_file.cc base/unix_file/random_access_file_utils.cc base/utils.cc
    base/zip_archive.cc)
target_sources(art_base PRIVATE ${ARTBASE_OPERATOR_OUT} ${AOSP}/tinyxml2/tinyxml2.cpp)
target_include_directories(art_base PUBLIC ${AOSP}/art/libartbase ${AOSP}/tinyxml2)
target_link_libraries(art_base PUBLIC art_flags art_palette aosp_ziparchive aosp_base aosp_log)

art_operator_out(DEXFILE_OPERATOR_OUT libdexfile
    dex/dex_file.h dex/dex_file_layout.h dex/dex_instruction.h dex/dex_instruction_utils.h
    dex/invoke_type.h)
aosp_library(art_dexfile art/libdexfile
    dex/art_dex_file_loader.cc dex/compact_dex_file.cc dex/compact_offset_table.cc
    dex/descriptors_names.cc dex/dex_file.cc dex/dex_file_exception_helpers.cc
    dex/dex_file_layout.cc dex/dex_file_loader.cc dex/dex_file_tracking_registrar.cc
    dex/dex_file_verifier.cc dex/dex_instruction.cc dex/modifiers.cc dex/primitive.cc
    dex/signature.cc dex/standard_dex_file.cc dex/type_lookup_table.cc dex/utf.cc)
target_sources(art_dexfile PRIVATE ${DEXFILE_OPERATOR_OUT})
target_include_directories(art_dexfile PUBLIC ${AOSP}/art/libdexfile)
target_link_libraries(art_dexfile PUBLIC art_base)

# --- ART runtime (libart) ---------------------------------------------------------------
include(art_runtime_sources.cmake)

aosp_library(aosp_lz4 lz4/lib lz4.c lz4hc.c lz4frame.c xxhash.c)
target_include_directories(aosp_lz4 PUBLIC ${AOSP}/lz4/lib)

aosp_library(aosp_lzma lzma/C
    7zCrc.c 7zCrcOpt.c Alloc.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c LzFind.c Lzma2Dec.c
    LzmaDec.c LzmaEnc.c Lzma2Enc.c Sha256.c Sha256Opt.c Xz.c XzCrc64.c XzCrc64Opt.c XzDec.c
    XzEnc.c XzIn.c)
target_include_directories(aosp_lzma PUBLIC ${AOSP}/lzma/C)
target_compile_definitions(aosp_lzma PUBLIC _7ZIP_ST Z7_ST)   # single-threaded codec

aosp_library(aosp_cpu_features cpu_features/src impl_aarch64_macos_or_iphone.c)
target_include_directories(aosp_cpu_features PUBLIC ${AOSP}/cpu_features/include)
target_compile_definitions(aosp_cpu_features PRIVATE HAVE_SYSCTLBYNAME=1 STACK_LINE_READER_BUFFER_SIZE=1024)

aosp_library(art_elffile art/libelffile
    elf/xz_utils.cc stream/buffered_output_stream.cc stream/file_output_stream.cc
    stream/output_stream.cc stream/vector_output_stream.cc)
target_include_directories(art_elffile PUBLIC ${AOSP}/art/libelffile)
target_link_libraries(art_elffile PUBLIC art_base aosp_lzma)

aosp_library(art_profile art/libprofile
    profile/profile_boot_info.cc profile/profile_compilation_info.cc)
target_include_directories(art_profile PUBLIC ${AOSP}/art/libprofile)
target_link_libraries(art_profile PUBLIC art_dexfile)

add_library(art_sigchain STATIC art-ios/sigchain_darwin.cc)
target_include_directories(art_sigchain PUBLIC ${AOSP}/art/sigchainlib)
target_link_libraries(art_sigchain PRIVATE art_flags aosp_log)

aosp_library(art_nativebridge art/libnativebridge native_bridge.cc)
target_include_directories(art_nativebridge PUBLIC ${AOSP}/art/libnativebridge/include)
target_link_libraries(art_nativebridge PRIVATE art_flags aosp_base aosp_log art_palette)

aosp_library(art_nativeloader art/libnativeloader native_loader.cpp)
target_include_directories(art_nativeloader
    PUBLIC ${AOSP}/art/libnativeloader/include
    PRIVATE ${AOSP}/libnativehelper/header_only_include ${AOSP}/libnativehelper/include_jni)
target_link_libraries(art_nativeloader PRIVATE art_base art_nativebridge)

# asm_defines.h: struct offsets for the assembly entrypoints, extracted from the compiler's
# assembly output for asm_defines.cc (the -S makes the "object" file textual assembly).
add_library(art_asm_defines_s OBJECT ${AOSP}/art/tools/cpp-define-generator/asm_defines.cc)
target_compile_options(art_asm_defines_s PRIVATE -S)
target_link_libraries(art_asm_defines_s PRIVATE art_runtime_headers)
set(ART_GEN ${CMAKE_CURRENT_BINARY_DIR}/art_gen)
file(MAKE_DIRECTORY ${ART_GEN})
# Darwin has no <elf.h>; musl's is self-contained and is what ART's musl host builds use.
configure_file(${AOSP}/musl/include/elf.h ${ART_GEN}/elf.h COPYONLY)
add_custom_command(
    OUTPUT ${ART_GEN}/asm_defines.h
    COMMAND ${Python3_EXECUTABLE} ${AOSP}/art/tools/cpp-define-generator/make_header.py
            $<TARGET_OBJECTS:art_asm_defines_s> > ${ART_GEN}/asm_defines.h
    DEPENDS art_asm_defines_s $<TARGET_OBJECTS:art_asm_defines_s>
    VERBATIM)
add_custom_target(art_asm_defines DEPENDS ${ART_GEN}/asm_defines.h)

# Include paths and defines for code that is compiled as part of libart.
add_library(art_runtime_headers INTERFACE)
target_include_directories(art_runtime_headers INTERFACE
    ${AOSP}/art/runtime
    ${AOSP}/art/cmdline
    ${AOSP}/art/compiler
    ${AOSP}/art/compiler/export
    ${AOSP}/art/odrefresh/include
    ${AOSP}/art/tools/cpp-define-generator
    ${AOSP}/art/sigchainlib
    ${AOSP}/art/libnativebridge/include
    ${AOSP}/art/libnativeloader/include
    ${AOSP}/libnativehelper/include
    ${AOSP}/libnativehelper/include_jni
    ${AOSP}/libnativehelper/header_only_include
    ${AOSP}/libnativehelper/include_platform
    ${AOSP}/libnativehelper/include_platform_header_only
    ${AOSP}/cpu_features/include
    ${AOSP}/dlmalloc
    ${ART_GEN}
    art-ios)
target_compile_definitions(art_runtime_headers INTERFACE BUILDING_LIBART)
target_link_libraries(art_runtime_headers INTERFACE
    art_flags art_dexfile art_elffile art_profile aosp_lz4)

# Linux-only implementation files, replaced by the Darwin sources in art-ios/.
list(REMOVE_ITEM ART_RUNTIME_COMMON_SRCS
    backtrace_helper.cc gc/collector/mark_compact.cc)

# Assembly entrypoints, in two steps because the sources are written for ELF:
#  1. tools/asm_strip_cfi.py copies them without unwind annotations Mach-O cannot express;
#  2. they are assembled with their ELF symbol names, and tools/macho_prefix_symbols.py
#     then renames the symbols in each object to their Mach-O spelling.
enable_language(ASM)
set(ART_ASM_DIR ${ART_GEN}/asm)
file(MAKE_DIRECTORY ${ART_ASM_DIR}/arch/arm64)
set(ART_ASM_STRIPPED)
foreach(rel
        arch/arm64/asm_support_arm64.S arch/arm64/quick_entrypoints_arm64.S
        arch/arm64/jni_entrypoints_arm64.S arch/arm64/memcmp16_arm64.S
        arch/quick_alloc_entrypoints.S arch/quick_field_entrypoints.S)
    add_custom_command(
        OUTPUT ${ART_ASM_DIR}/${rel}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_strip_cfi.py
                ${AOSP}/art/runtime/${rel} ${ART_ASM_DIR}/${rel}
        DEPENDS ${AOSP}/art/runtime/${rel} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_strip_cfi.py
        VERBATIM)
    list(APPEND ART_ASM_STRIPPED ${ART_ASM_DIR}/${rel})
endforeach()
add_custom_target(art_asm_sources DEPENDS ${ART_ASM_STRIPPED})

set(ART_ASM_UNITS quick_entrypoints_arm64 jni_entrypoints_arm64 memcmp16_arm64)
list(TRANSFORM ART_ASM_UNITS REPLACE "(.+)" "${ART_ASM_DIR}/arch/arm64/\\1.S" OUTPUT_VARIABLE ART_ASM_UNIT_SRCS)
add_library(art_runtime_asm OBJECT ${ART_ASM_UNIT_SRCS})
# The stripped copies must shadow the originals for #include "arch/....S".
target_include_directories(art_runtime_asm BEFORE PRIVATE ${ART_ASM_DIR} ${AOSP}/art/runtime/arch/arm64)
target_link_libraries(art_runtime_asm PRIVATE art_runtime_headers)
add_dependencies(art_runtime_asm art_asm_defines art_asm_sources)
set(ART_ASM_OBJECTS)
foreach(name ${ART_ASM_UNITS})
    set(out ${ART_GEN}/${name}.o)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/macho_prefix_symbols.py
                $<FILTER:$<TARGET_OBJECTS:art_runtime_asm>,INCLUDE,/${name}\\.S> ${out}
        DEPENDS art_runtime_asm $<TARGET_OBJECTS:art_runtime_asm>
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/macho_prefix_symbols.py
        VERBATIM)
    set_source_files_properties(${out} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND ART_ASM_OBJECTS ${out})
endforeach()

art_operator_out(ART_RUNTIME_OPERATOR_OUT runtime ${ART_RUNTIME_OPERATOR_HDRS})
aosp_library(art_runtime art/runtime
    ${ART_RUNTIME_COMMON_SRCS}
    arch/arm64/context_arm64.cc
    arch/arm64/entrypoints_init_arm64.cc
    arch/arm64/thread_arm64.cc
    monitor_pool.cc
    monitor_linux.cc
    runtime_linux.cc
    thread_linux.cc)
target_sources(art_runtime PRIVATE ${ART_RUNTIME_OPERATOR_OUT} ${ART_ASM_OBJECTS}
    ${AOSP}/art/runtime/interpreter/mterp/nterp.cc
    art-ios/backtrace_darwin.cc
    art-ios/fault_handler_darwin.cc
    art-ios/futex_darwin.cc
    art-ios/nterp_stub.cc
    art-ios/unavailable_darwin.cc
    art-ios/android_abi_jni.cc
    art-ios/android_abi_jni_arm64.S)
add_dependencies(art_runtime art_asm_defines)
target_link_libraries(art_runtime PUBLIC
    art_runtime_headers art_sigchain art_nativebridge art_nativeloader art_palette
    aosp_cpu_features aosp_ziparchive aosp_base aosp_log z)

include(libcore.cmake)
include(framework.cmake)
