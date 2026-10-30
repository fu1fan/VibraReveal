# ELF2 / RK3588（AArch64）交叉编译工具链文件。
#
# 用法：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=/opt/elf2/cmake/elf2-aarch64-toolchain.cmake
#
# 工具链前缀优先取环境变量 ELF2_CROSS_COMPILE（登录 shell 由 /etc/profile.d/elf2.sh 设置），
# 未设置时按镜像内的固定依赖目录探测。

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_elf2_prefix "")
if(DEFINED ENV{ELF2_CROSS_COMPILE} AND NOT "$ENV{ELF2_CROSS_COMPILE}" STREQUAL "")
  set(_elf2_prefix "$ENV{ELF2_CROSS_COMPILE}")
else()
  file(GLOB _elf2_gccs /opt/elf2-deps/prebuilts/gcc/linux-x86/aarch64/*/bin/aarch64-none-linux-gnu-gcc)
  list(LENGTH _elf2_gccs _elf2_count)
  if(_elf2_count LESS 1)
    message(FATAL_ERROR "未找到工具链：请使用 elf2-sdk run cmake，或进入完整系统环境")
  endif()
  list(GET _elf2_gccs 0 _elf2_gcc)
  get_filename_component(_elf2_bin "${_elf2_gcc}" DIRECTORY)
  set(_elf2_prefix "${_elf2_bin}/aarch64-none-linux-gnu-")
endif()

set(CMAKE_C_COMPILER "${_elf2_prefix}gcc")
set(CMAKE_CXX_COMPILER "${_elf2_prefix}g++")

find_program(ELF2_CCACHE ccache)
if(ELF2_CCACHE)
  set(CMAKE_C_COMPILER_LAUNCHER "${ELF2_CCACHE}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${ELF2_CCACHE}")
endif()

if(DEFINED ENV{ELF2_SYSROOT} AND NOT "$ENV{ELF2_SYSROOT}" STREQUAL "")
  set(CMAKE_SYSROOT "$ENV{ELF2_SYSROOT}")
  set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ELF2_CROSS_COMPILE_PREFIX "${_elf2_prefix}" CACHE INTERNAL "ELF2 交叉工具链前缀")
