# 命令行工具/客户端样例

# rtmp_push: 把 FLV 文件以 RTMP 推流到服务器
add_executable(rtmp_push rtmp_push.cpp)
target_link_libraries(rtmp_push PRIVATE streaming_core Threads::Threads)
if(OpenSSL_FOUND)
    target_link_libraries(rtmp_push PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# whip_client: 通过 WHIP（WebRTC）把 FLV 文件推流到服务器
add_executable(whip_client whip_client.cpp)
target_link_libraries(whip_client PRIVATE streaming_core Threads::Threads)
if(OpenSSL_FOUND)
    target_link_libraries(whip_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
