#include "core/Buffer.hpp"
#include "rtmp/Chunk.hpp"
#include "test_framework.hpp"

#include <cstring>
#include <vector>

using namespace streaming;

static RtmpMessage makeMsg(uint8_t type, uint32_t csid, uint32_t ts,
                           uint32_t sid, size_t payloadLen) {
    RtmpMessage m;
    m.typeId = type;
    m.chunkStreamId = csid;
    m.timestamp = ts;
    m.streamId = sid;
    m.payload.assign(payloadLen, 0xAB);
    for (size_t i = 0; i < payloadLen; ++i) m.payload[i] = uint8_t(i & 0xFF);
    return m;
}

TEST(chunk_single_roundtrip) {
    Buffer buf;
    ChunkWriter writer(128);
    RtmpMessage src = makeMsg(9, 6, 100, 1, 50);
    writer.write(buf, src);

    ChunkParser parser(128);
    RtmpMessage out;
    ASSERT_TRUE(parser.parseMessage(buf, out));
    ASSERT_EQ(out.typeId, uint8_t(9));
    ASSERT_EQ(out.chunkStreamId, uint32_t(0));  // parser 未回填 csid
    ASSERT_EQ(out.streamId, uint32_t(1));
    ASSERT_EQ(out.timestamp, uint32_t(100));
    ASSERT_EQ(out.payload.size(), size_t(50));
    ASSERT_EQ(out.payload[0], uint8_t(0));
    ASSERT_EQ(out.payload[49], uint8_t(49));
}

TEST(chunk_multi_chunk) {
    Buffer buf;
    const uint32_t kChunkSize = 16;
    ChunkWriter writer(kChunkSize);
    RtmpMessage src = makeMsg(8, 4, 50, 1, 50);
    writer.write(buf, src);

    ChunkParser parser(kChunkSize);
    RtmpMessage out;
    ASSERT_TRUE(parser.parseMessage(buf, out));
    ASSERT_EQ(out.typeId, uint8_t(8));
    ASSERT_EQ(out.streamId, uint32_t(1));
    ASSERT_EQ(out.payload.size(), size_t(50));
    ASSERT_EQ(out.payload[49], uint8_t(49));
}

TEST(chunk_extended_timestamp) {
    Buffer buf;
    ChunkWriter writer(128);
    RtmpMessage src = makeMsg(9, 6, 0x1000000, 1, 10);
    writer.write(buf, src);

    ChunkParser parser(128);
    RtmpMessage out;
    ASSERT_TRUE(parser.parseMessage(buf, out));
    ASSERT_EQ(out.timestamp, uint32_t(0x1000000));
    ASSERT_EQ(out.payload.size(), size_t(10));
}

TEST(chunk_partial_feed) {
    Buffer full;
    ChunkWriter writer(128);
    RtmpMessage src = makeMsg(20, 3, 0, 0, 20);
    writer.write(full);

    // 一次喂 1 字节，状态机最终应能拼出消息
    ChunkParser parser(128);
    RtmpMessage out;
    bool got = false;
    while (full.readableBytes() > 0 && !got) {
        char c = full.peek()[0];
        full.retrieve(1);
        Buffer one;
        one.append(&c, 1);
        got = parser.parseMessage(one, out);
    }
    ASSERT_TRUE(got);
    ASSERT_EQ(out.typeId, uint8_t(20));
    ASSERT_EQ(out.payload.size(), size_t(20));
}
