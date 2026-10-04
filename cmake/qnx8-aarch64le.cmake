# Toolchain file for cross-compiling fs-9p with QNX SDP 8.0, aarch64le target.
# Requires the SDP environment: source ~/qnx800/qnxsdp-env.sh

if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
    message(FATAL_ERROR "QNX_HOST/QNX_TARGET not set; source qnxsdp-env.sh first")
endif()

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 8.0.0)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(QNX_HOST   "$ENV{QNX_HOST}")
set(QNX_TARGET "$ENV{QNX_TARGET}")

set(CMAKE_C_COMPILER   "${QNX_HOST}/usr/bin/qcc")
set(CMAKE_CXX_COMPILER "${QNX_HOST}/usr/bin/q++")
# CMake's QCC module turns *_COMPILER_TARGET into "-V<target>". Target name
# verified with `qcc -V` on SDP 8.0 (gcc 12.2.0).
set(CMAKE_C_COMPILER_TARGET   gcc_ntoaarch64le)
set(CMAKE_CXX_COMPILER_TARGET gcc_ntoaarch64le_cxx)

set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}" "${QNX_TARGET}/aarch64le")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(FS9P_USEMSG "${QNX_HOST}/usr/bin/usemsg" CACHE FILEPATH "usemsg tool")
