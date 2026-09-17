# 单元测试：内置最小测试框架（避免 GoogleTest 外部依赖）

add_executable(unit_tests
    test_main.cpp
    test_buffer.cpp
    test_lru.cpp
    test_ring_buffer.cpp
    test_ts_packet.cpp
    test_pes.cpp
    test_amf.cpp
    test_chunk.cpp
    test_http_parser.cpp
    test_router.cpp
    test_jwt.cpp
    test_m3u8.cpp
    test_consistent_hash.cpp
    test_rate_limiter.cpp
    test_circuit_breaker.cpp
    test_srt_protocol.cpp
    test_vhost.cpp
    test_json.cpp
    test_parsers.cpp
    test_config.cpp
    test_system_stats.cpp
    test_transcode_tasks.cpp
    test_redis_protocol.cpp
)
target_link_libraries(unit_tests PRIVATE streaming_core)
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
