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
    writer.write(full, src);

    // 一次喂 1 字节，状态机最终应能拼出消息
    ChunkParser parser(128);
    RtmpMessage out;
    bool got = false;
    Buffer in;  // 与连接输入缓冲一致：逐字节追加到同一 Buffer
    while (full.readableBytes() > 0 && !got) {
        char c = full.peek()[0];
        full.retrieve(1);
        in.append(&c, 1);
        got = parser.parseMessage(in, out);
    }
    ASSERT_TRUE(got);
    ASSERT_EQ(out.typeId, uint8_t(20));
    ASSERT_EQ(out.payload.size(), size_t(20));
}

// 回归：头已到达但负载未到齐时不得消费任何字节（原实现会错位）
TEST(chunk_header_then_payload_split) {
    ChunkWriter writer(128);
    Buffer full;
    RtmpMessage a = makeMsg(9, 6, 40, 1, 300);  // 跨 3 个 chunk
    RtmpMessage b = makeMsg(8, 4, 41, 1, 50);
    writer.write(full, a);
    writer.write(full, b);
    std::string wire = full.retrieveAllAsString();

    // 以各种切分点分两段喂入，结果都应得到 a、b 两条完整消息
    for (size_t cut = 1; cut < wire.size(); ++cut) {
        ChunkParser parser(128);
        Buffer in;
        std::vector<RtmpMessage> got;
        RtmpMessage m;
        in.append(wire.data(), cut);
        while (parser.parseMessage(in, m)) { got.push_back(m); m = RtmpMessage{}; }
        in.append(wire.data() + cut, wire.size() - cut);
        while (parser.parseMessage(in, m)) { got.push_back(m); m = RtmpMessage{}; }
        ASSERT_FALSE(parser.hasError());
        ASSERT_EQ(got.size(), size_t(2));
        ASSERT_EQ(got[0].payload.size(), size_t(300));
        ASSERT_EQ(got[0].timestamp, uint32_t(40));
        ASSERT_EQ(got[1].payload.size(), size_t(50));
        ASSERT_EQ(got[1].typeId, uint8_t(8));
        ASSERT_EQ(in.readableBytes(), size_t(0));
    }
}

// 扩展时间戳 + 续传 chunk 也携带扩展时间戳（FFmpeg/OBS 行为）
TEST(chunk_extended_timestamp_continuation) {
    ChunkWriter writer(128);
    Buffer buf;
    RtmpMessage a = makeMsg(9, 6, 0x1234567, 1, 400);
    writer.write(buf, a);
    ChunkParser parser(128);
    RtmpMessage out;
    ASSERT_TRUE(parser.parseMessage(buf, out));
    ASSERT_EQ(out.timestamp, uint32_t(0x1234567));
    ASSERT_EQ(out.payload.size(), size_t(400));
    ASSERT_EQ(buf.readableBytes(), size_t(0));
}

// fmt3 起新消息（沿用上一条头与 delta）
TEST(chunk_fmt3_new_message) {
    Buffer buf;
    // fmt0 csid=4 ts=100 len=2 type=8 sid=1
    const uint8_t m1[] = {0x04, 0, 0, 100, 0, 0, 2, 8, 1, 0, 0, 0, 0xAA, 0xBB};
    // fmt2 csid=4 delta=20
    const uint8_t m2[] = {0x84, 0, 0, 20, 0xCC, 0xDD};
    // fmt3 csid=4：新消息，沿用 delta=20
    const uint8_t m3[] = {0xC4, 0xEE, 0xFF};
    buf.append(reinterpret_cast<const char*>(m1), sizeof(m1));
    buf.append(reinterpret_cast<const char*>(m2), sizeof(m2));
    buf.append(reinterpret_cast<const char*>(m3), sizeof(m3));
    ChunkParser parser(128);
    RtmpMessage out;
    ASSERT_TRUE(parser.parseMessage(buf, out));
    ASSERT_EQ(out.timestamp, uint32_t(100));
    ASSERT_TRUE(parser.parseMessage(buf, out));
    ASSERT_EQ(out.timestamp, uint32_t(120));
    ASSERT_TRUE(parser.parseMessage(buf, out));
    ASSERT_EQ(out.timestamp, uint32_t(140));
    ASSERT_EQ(out.payload[0], uint8_t(0xEE));
}

// 未知 csid 以 fmt3 开头属于协议错误
TEST(chunk_invalid_fmt3_start) {
    Buffer buf;
    const uint8_t bad[] = {0xC5, 0x00};
    buf.append(reinterpret_cast<const char*>(bad), sizeof(bad));
    ChunkParser parser(128);
    RtmpMessage out;
    ASSERT_FALSE(parser.parseMessage(buf, out));
    ASSERT_TRUE(parser.hasError());
}
