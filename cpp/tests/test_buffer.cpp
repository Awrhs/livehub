#include "core/Buffer.hpp"
#include "test_framework.hpp"

#include <cstring>

using namespace streaming;

TEST(buffer_append_retrieve) {
    Buffer buf;
    buf.append("hello", 5);
    ASSERT_EQ(buf.readableBytes(), size_t(5));
    std::string s = buf.retrieveAsString(5);
    ASSERT_EQ(s, std::string("hello"));
    ASSERT_EQ(buf.readableBytes(), size_t(0));
}

TEST(buffer_big_endian) {
    Buffer buf;
    buf.appendInt32(0x12345678);
    buf.appendInt16(0x1234);
    buf.appendInt8(0x7F);
    ASSERT_EQ(buf.readInt32(), int32_t(0x12345678));
    ASSERT_EQ(buf.readInt16(), int16_t(0x1234));
    ASSERT_EQ(buf.readInt8(), int8_t(0x7F));
}

TEST(buffer_find_crlf) {
    Buffer buf;
    buf.append("GET / HTTP/1.1\r\nHost: x\r\n\r\n", 28);
    const char* p = buf.findCRLF();
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(std::string(p, 2), std::string("\r\n"));
}

TEST(buffer_growth) {
    Buffer buf(4);
    for (int i = 0; i < 1000; ++i) {
        buf.append("abcdef", 6);
    }
    ASSERT_EQ(buf.readableBytes(), size_t(6000));
}

TEST(buffer_prepend_small) {
    Buffer buf;
    buf.append("world", 5);
    // 前置预留区有 kCheapPrepend(8) 字节，prepend 8 字节以内应直接前移
    buf.prepend("hello", 5);
    ASSERT_EQ(std::string(buf.peek(), buf.readableBytes()),
              std::string("helloworld"));
}

TEST(buffer_prepend_large_grows) {
    Buffer buf;
    buf.append("data", 4);
    // 超过预留区（8 字节）时必须扩容/移位并真正写入
    const char* prefix = "long-prefix-";
    buf.prepend(prefix, std::strlen(prefix));
    std::string expected = std::string(prefix) + "data";
    ASSERT_EQ(buf.readableBytes(), expected.size());
    ASSERT_EQ(std::string(buf.peek(), buf.readableBytes()), expected);
}
