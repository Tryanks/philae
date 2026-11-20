cmake_minimum_required(VERSION 3.24)
project(a2i_runtime LANGUAGES C CXX OBJC OBJCXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS ON)   # AOSP code is gnu++20

# Executables find the runtime's dylibs in the bundle's Frameworks directory.
# (Install-style run paths, so no absolute build directory ends up in the binaries.)
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
set(CMAKE_INSTALL_RPATH "@executable_path/Frameworks;@loader_path")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Frameworks)

add_subdirectory(aosp)

# Runtime bootstrap shared by the app host and the command-line driver.
add_library(a2i_vm STATIC vm/android_vm.cc)
target_include_directories(a2i_vm PUBLIC vm ${CMAKE_SOURCE_DIR}/../third_party/aosp/libnativehelper/include_jni)
target_compile_options(a2i_vm PRIVATE -Wall -Wextra)
target_link_libraries(a2i_vm PRIVATE art)
# Loaded by name when the runtime starts, so nothing links against them.
add_dependencies(a2i_vm icu_jni javacore openjdk javacrypto android_framework stats_jni media_jni
    connectivity_jni a2i_bionic)

# See scripts/prepare-prebuilt-runtime.sh.
add_library(a2i_macshim SHARED simulator/macshim.c)
set_target_properties(a2i_macshim PROPERTIES
    MACOSX_RPATH ON BUILD_WITH_INSTALL_NAME_DIR ON INSTALL_NAME_DIR "@rpath")
add_dependencies(a2i_vm a2i_macshim)

add_executable(a2i-vm tools/vm_main.cc)
set_target_properties(a2i-vm PROPERTIES MACOSX_BUNDLE OFF)
target_compile_options(a2i-vm PRIVATE -Wall -Wextra)
target_link_options(a2i-vm PRIVATE -Wl,-no_warn_duplicate_libraries)
target_link_libraries(a2i-vm PRIVATE a2i_vm)

add_executable(A2IHost host/main.m host/webview.m)
target_compile_options(A2IHost PRIVATE -Wall -Wextra -fobjc-arc)
# CMake defaults to wrapping iOS executables in a .app; the packer needs the bare binary.
set_target_properties(A2IHost PROPERTIES MACOSX_BUNDLE OFF)
target_link_options(A2IHost PRIVATE -Wl,-no_warn_duplicate_libraries)
target_link_libraries(A2IHost PRIVATE
    a2i_vm
    "-framework UIKit" "-framework Foundation" "-framework CoreGraphics" "-framework QuartzCore"
    "-framework WebKit")
