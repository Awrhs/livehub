// Buffer append/retrieve 吞吐量基准。
// 用法：./bench_buffer [iterations]
#include "core/Buffer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace streaming;

int main(int argc, char** argv) {
    int iterations = argc > 1 ? std::atoi(argv[1]) : 10'000'000;
    const size_t block = 1024;
    std::string payload(block, 'x');

    Buffer buf(4096);
    auto t0 = std::chrono::high_resolution_clock::now();
    size_t bytes = 0;
    for (int i = 0; i < iterations; ++i) {
        buf.append(payload);
        bytes += block;
        // 模拟读出一半，保持 buffer 不膨胀
        if (buf.readableBytes() >= 8 * block) {
            buf.retrieve(4 * block);
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("Buffer: %d iters, %.2f Mops, %.2f GB/s, %.3f sec\n",
                iterations, iterations / sec / 1e6,
                bytes / sec / 1e9, sec);
    return 0;
}
