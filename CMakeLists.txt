cmake_minimum_required(VERSION 3.20)
project(lr2native C)
if(WIN32)
  enable_language(RC)
endif()

# Generated sources (the recompiled game, including the image's data
# sections) come from tools/recomp/recomp.py; see README.md.
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Directory with recompiled sources")
if(NOT EXISTS "${GEN_DIR}/gen_files.cmake")
  message(FATAL_ERROR "No recompiled sources in ${GEN_DIR}. Run tools/recomp/recomp.py first.")
endif()
include("${GEN_DIR}/gen_files.cmake")

find_package(SDL3 REQUIRED CONFIG)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.c)
# runtime/src/os/os_<platform>.c compile to nothing on other platforms.

# The generated code is huge and machine-written: optimize, keep warnings quiet.
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-w;-fno-strict-aliasing")

# zlib (savestate files), built from the vendored source.
set(ZLIB_DIR "${CMAKE_SOURCE_DIR}/third_party/zlib")
# zlib's own CMake renames zconf.h to zconf.h.included in the source tree.
if(EXISTS "${ZLIB_DIR}/zconf.h")
  configure_file("${ZLIB_DIR}/zconf.h" "${CMAKE_BINARY_DIR}/zlib/zconf.h" COPYONLY)
else()
  configure_file("${ZLIB_DIR}/zconf.h.included" "${CMAKE_BINARY_DIR}/zlib/zconf.h" COPYONLY)
endif()
add_library(lr2_zlib STATIC
  ${ZLIB_DIR}/adler32.c ${ZLIB_DIR}/crc32.c ${ZLIB_DIR}/deflate.c ${ZLIB_DIR}/inffast.c
  ${ZLIB_DIR}/inflate.c ${ZLIB_DIR}/inftrees.c ${ZLIB_DIR}/trees.c ${ZLIB_DIR}/zutil.c)
target_include_directories(lr2_zlib PUBLIC "${CMAKE_BINARY_DIR}/zlib" "${ZLIB_DIR}")
target_compile_options(lr2_zlib PRIVATE -O2 -w)

add_executable(lr2 ${RUNTIME_SOURCES} ${GEN_SOURCES})
target_include_directories(lr2 PRIVATE runtime/include runtime/src ${GEN_DIR})
target_compile_options(lr2 PRIVATE -Wall -Wno-unused-function -fno-strict-aliasing)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(lr2 PRIVATE -Wno-format-truncation)
endif()
if(WIN32)
  # MinGW-w64 (llvm-mingw or MSYS2 clang).  runtime/compat/win32 supplies
  # the few POSIX headers MinGW lacks; runtime/src/os/os_win32.c the rest.
  target_include_directories(lr2 BEFORE PRIVATE runtime/compat/win32)
  target_compile_definitions(lr2 PRIVATE _FILE_OFFSET_BITS=64)
  target_sources(lr2 PRIVATE runtime/res/lr2.rc)
  # A self-contained lr2.exe: static SDL3, winpthreads and compiler runtime;
  # only Windows' own DLLs (UCRT, opengl32, ...) are needed.
  option(LR2_STATIC "Link SDL3 and the MinGW runtime statically" ON)
  if(LR2_STATIC AND TARGET SDL3::SDL3-static)
    target_link_libraries(lr2 PRIVATE SDL3::SDL3-static)
    target_link_options(lr2 PRIVATE -static)
  else()
    target_link_libraries(lr2 PRIVATE SDL3::SDL3)
  endif()
  target_link_libraries(lr2 PRIVATE lr2_zlib pthread winmm)
  set_target_properties(lr2 PROPERTIES WIN32_EXECUTABLE ON)
  # Deep guest recursion runs on the host stack (Windows' default is 1 MiB;
  # other threads get 8 MiB in sync.c).  Exporting every function lets
  # dladdr() name them for backtraces and savestate call chains.
  target_link_options(lr2 PRIVATE -Wl,--stack,16777216 -Wl,--export-all-symbols)
else()
  target_link_libraries(lr2 PRIVATE SDL3::SDL3 lr2_zlib m pthread)
  # Export symbols so backtraces name the recompiled guest functions.
  set_target_properties(lr2 PROPERTIES ENABLE_EXPORTS ON)
endif()
if(APPLE)
  target_compile_definitions(lr2 PRIVATE GL_SILENCE_DEPRECATION)
endif()
