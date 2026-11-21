cmake_minimum_required(VERSION 3.16)
project(streaming_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认 Release，开发可 -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 选项
option(ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)
option(ENABLE_TESTS "Build self-contained unit tests" ON)
option(ENABLE_BENCHMARKS "Build benchmarks" OFF)

# 编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

option(ENABLE_WERROR "Treat compiler warnings as errors" OFF)
if(ENABLE_WERROR)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
endif()

option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
    message(STATUS "ThreadSanitizer enabled")
endif()

if(ENABLE_ASAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
    message(STATUS "AddressSanitizer + UBSan enabled")
endif()

# 输出目录
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 查找系统依赖
find_package(Threads REQUIRED)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    add_compile_definitions(HAVE_OPENSSL=1)
else()
    message(STATUS "OpenSSL NOT found - JWT will use a stub HMAC")
endif()

# 查找 FFmpeg (libav) - 阶段五才需要
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(AVFORMAT QUIET libavformat)
    pkg_check_modules(AVCODEC QUIET libavcodec)
    pkg_check_modules(AVUTIL QUIET libavutil)
    pkg_check_modules(SWRESAMPLE QUIET libswresample)
    pkg_check_modules(SWSCALE QUIET libswscale)
    if(AVFORMAT_FOUND AND AVCODEC_FOUND AND AVUTIL_FOUND AND SWRESAMPLE_FOUND AND SWSCALE_FOUND)
        message(STATUS "FFmpeg libav found")
        add_compile_definitions(HAVE_LIBAV=1)
    else()
        message(STATUS "FFmpeg libav NOT found - transcode module will be stubbed")
    endif()
endif()

# 核心库
add_subdirectory(src)

# 测试
if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

option(ENABLE_TOOLS "Build command-line tools (rtmp_push, whip_client)" ON)
if(ENABLE_TOOLS)
    add_subdirectory(tools)
endif()

message(STATUS "")
message(STATUS "=== streaming_server C++ build configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "ASan: ${ENABLE_ASAN}")
message(STATUS "Tests: ${ENABLE_TESTS}")
message(STATUS "================================================")
