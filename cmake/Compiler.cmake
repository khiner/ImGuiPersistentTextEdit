# Homebrew LLVM, for C++26 and libc++.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_OSX_SYSROOT macosx CACHE STRING "macOS SDK" FORCE)
    set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang" CACHE FILEPATH "LLVM compiler" FORCE)
    set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++" CACHE FILEPATH "LLVM compiler" FORCE)
endif()
