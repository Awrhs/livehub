# 核心流媒体服务器库

# 收集所有模块的源文件
file(GLOB_RECURSE CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/net/*.cpp"
)

# 阶段二之后逐步加入的模块（文件存在时才包含）
file(GLOB_RECURSE MODULE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/api/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hls/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cache/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rtmp/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/transcode/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cluster/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/middleware/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/auth/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/metrics/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rtsp/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dvr/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/webhook/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/srt/*.cpp"
)

# 静态库：流媒体核心
add_library(streaming_core STATIC ${CORE_SOURCES} ${MODULE_SOURCES})

target_include_directories(streaming_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(streaming_core
    PUBLIC
        Threads::Threads
)

# OpenSSL 链接（JWT 用）
if(OpenSSL_FOUND)
    target_link_libraries(streaming_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# libav 链接（转码用，阶段五）
if(AVFORMAT_FOUND AND AVCODEC_FOUND AND AVUTIL_FOUND AND SWRESAMPLE_FOUND AND SWSCALE_FOUND)
    target_include_directories(streaming_core PUBLIC ${AVFORMAT_INCLUDE_DIRS})
    target_link_libraries(streaming_core PUBLIC ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES} ${SWRESAMPLE_LIBRARIES} ${SWSCALE_LIBRARIES})
endif()

# 主可执行文件：流媒体服务器
add_executable(streaming_server main.cpp)
target_link_libraries(streaming_server PRIVATE streaming_core)
