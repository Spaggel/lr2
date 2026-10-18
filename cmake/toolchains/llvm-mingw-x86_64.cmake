# Windows x64 with llvm-mingw.  See llvm-mingw.cmake.
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(LR2_MINGW_TRIPLE x86_64-w64-mingw32)
include("${CMAKE_CURRENT_LIST_DIR}/llvm-mingw.cmake")
