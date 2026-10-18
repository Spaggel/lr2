# Cross-compiling for Windows with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw), shared by the per-architecture
# toolchain files.  The llvm-mingw directory is taken from LLVM_MINGW (a
# CMake or environment variable); without it the tools must be on PATH.
if(NOT LLVM_MINGW AND DEFINED ENV{LLVM_MINGW})
  set(LLVM_MINGW "$ENV{LLVM_MINGW}")
endif()
# try_compile projects re-read this file and need the setting too.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW)

set(CMAKE_SYSTEM_NAME Windows)
set(_prefix "${LR2_MINGW_TRIPLE}-")
if(LLVM_MINGW)
  set(_prefix "${LLVM_MINGW}/bin/${LR2_MINGW_TRIPLE}-")
  set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW}/${LR2_MINGW_TRIPLE}")
endif()
set(CMAKE_C_COMPILER "${_prefix}clang")
set(CMAKE_CXX_COMPILER "${_prefix}clang++")
set(CMAKE_RC_COMPILER "${_prefix}windres")
set(CMAKE_AR "${_prefix}ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_prefix}ranlib" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Packages (SDL3) are found through <Pkg>_DIR or CMAKE_PREFIX_PATH.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
