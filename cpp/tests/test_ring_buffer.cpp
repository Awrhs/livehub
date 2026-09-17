#include "cache/RingBuffer.hpp"
#include "test_framework.hpp"

#include <cstring>
#include <thread>

using namespace streaming;

TEST(ring_buffer_write_read) {
    RingBuffer rb(1024);
    const char* msg = "abcdef";
    ASSERT_EQ(rb.write(msg, 6), size_t(6));
    ASSERT_EQ(rb.readableBytes(), size_t(6));
    char out[8] = {0};
    ASSERT_EQ(rb.read(out, 6), size_t(6));
    ASSERT_EQ(std::string(out, 6), std::string("abcdef"));
}

TEST(ring_buffer_wrap_around) {
    RingBuffer rb(16);
    char data[8] = {0};
    ASSERT_EQ(rb.write("12345678", 8), size_t(8));
    rb.read(data, 6);  // 留 2 字节
    // 写 8 字节触发回绕
    ASSERT_EQ(rb.write("ABCDEFGH", 8), size_t(8));
    ASSERT_EQ(rb.readableBytes(), size_t(10));
    char out[16] = {0};
    ASSERT_EQ(rb.read(out, 10), size_t(10));
    ASSERT_EQ(std::string(out, 10), std::string("78ABCDEFGH"));
}

TEST(ring_buffer_spsc_threaded) {
    RingBuffer rb(4096);
    const int N = 10000;
    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            int v = i;
            while (rb.write(&v, sizeof(v)) != sizeof(v)) {}
        }
    });
    std::thread consumer([&] {
        int got = 0;
        while (got < N) {
            int v = 0;
            if (rb.read(&v, sizeof(v)) == sizeof(v)) {
                ASSERT_EQ(v, got);
                ++got;
            }
        }
    });
    producer.join();
    consumer.join();
}
