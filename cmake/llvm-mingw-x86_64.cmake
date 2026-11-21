# Cross-compile with llvm-mingw (clang + lld) instead of GCC MinGW.
#
#     cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw-x86_64.cmake \
#       -DLLVM_MINGW_DIR=../toolchains/llvm-mingw-<version>-msvcrt-ubuntu-22.04-x86_64 \
#       -DXBOXRECOMP_DIR=../xboxrecomp
#
# Why: GCC's MinGW target only has emulated TLS, so every RECOMP_TLS guest
# register access in the generated code, and every NV_TLS access in the
# software rasteriser, is a call to __emutls_get_address. clang emits native
# Windows TLS (_tls_index / gs:[0x58]) instead.
# llvm-mingw: https://github.com/mstorsjo/llvm-mingw (use the msvcrt build, the
# same C runtime as Debian/Ubuntu's mingw-w64 GCC).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# CMake re-reads this file inside its try_compile projects; the environment
# carries the (absolute) path there.
if(NOT DEFINED LLVM_MINGW_DIR AND DEFINED ENV{LLVM_MINGW_DIR})
    set(LLVM_MINGW_DIR "$ENV{LLVM_MINGW_DIR}")
endif()
if(NOT DEFINED LLVM_MINGW_DIR)
    message(FATAL_ERROR "Set LLVM_MINGW_DIR to the extracted llvm-mingw folder")
endif()
get_filename_component(LLVM_MINGW_DIR "${LLVM_MINGW_DIR}" ABSOLUTE)
set(ENV{LLVM_MINGW_DIR} "${LLVM_MINGW_DIR}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_DIR)

set(CMAKE_C_COMPILER   ${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  ${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_AR           ${LLVM_MINGW_DIR}/bin/llvm-ar)
set(CMAKE_RANLIB       ${LLVM_MINGW_DIR}/bin/llvm-ranlib)

set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW_DIR}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
