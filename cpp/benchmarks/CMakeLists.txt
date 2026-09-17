# 基准测试：无外部依赖，用 std::chrono 输出 ops/sec

add_executable(bench_buffer bench_buffer.cpp)
target_link_libraries(bench_buffer PRIVATE streaming_core Threads::Threads)
target_include_directories(bench_buffer PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(bench_lru bench_lru.cpp)
target_link_libraries(bench_lru PRIVATE streaming_core Threads::Threads)
target_include_directories(bench_lru PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(bench_http bench_http.cpp)
target_link_libraries(bench_http PRIVATE streaming_core Threads::Threads)
target_include_directories(bench_http PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(bench_concurrent bench_concurrent.cpp)
target_link_libraries(bench_concurrent PRIVATE streaming_core Threads::Threads)
target_include_directories(bench_concurrent PRIVATE ${CMAKE_SOURCE_DIR}/include)
