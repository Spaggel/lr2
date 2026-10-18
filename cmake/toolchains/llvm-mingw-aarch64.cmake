# Windows on ARM64 with llvm-mingw.  See llvm-mingw.cmake.
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(LR2_MINGW_TRIPLE aarch64-w64-mingw32)
include("${CMAKE_CURRENT_LIST_DIR}/llvm-mingw.cmake")
