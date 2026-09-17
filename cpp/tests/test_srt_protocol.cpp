#include "srt/SrtProtocol.hpp"
#include "test_framework.hpp"

#include <arpa/inet.h>

#include <cstring>
#include <vector>

using namespace streaming;

TEST(srt_data_header_roundtrip) {
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint32_t seq = 0x12345678;
    uint32_t ts = 0x00ABCDEF;
    uint32_t srcSock = 0x00000007;
    auto pkt = srtBuildData(seq, ts, srcSock, payload, sizeof(payload));

    // 16 字节头 + 4 字节负载
    ASSERT_EQ(pkt.size(), size_t(20));
    // 数据包：最高位为 0
    ASSERT_FALSE(srtIsControl(pkt.data()));
    // 序号为低 31 位
    ASSERT_EQ(srtDataSeq(pkt.data()), seq & 0x7FFFFFFFu);
    // timestamp 字段（大端）
    uint32_t tsField = ntohl(*reinterpret_cast<const uint32_t*>(pkt.data() + 4));
    ASSERT_EQ(tsField, ts);
    // dest socket（大端）
    uint32_t dst = ntohl(*reinterpret_cast<const uint32_t*>(pkt.data() + 8));
    ASSERT_EQ(dst, srcSock);
    // 负载完整
    ASSERT_EQ(pkt[16], 0xDE);
    ASSERT_EQ(pkt[19], 0xEF);
}

TEST(srt_control_header_roundtrip) {
    uint32_t info = 0x00112233;
    uint32_t ts = 0x55667788;
    uint32_t destSock = 0x0BADF00D;
    auto pkt = srtBuildControl(SrtCtrlType::kAck, info, ts, destSock);

    ASSERT_EQ(pkt.size(), size_t(16));
    // 控制包：最高位为 1
    ASSERT_TRUE(srtIsControl(pkt.data()));
    // type 位于 field0 的 bit16..30（大端 bytes 0..1 的低 15 位）
    ASSERT_EQ(srtCtrlType(pkt.data()), static_cast<uint16_t>(SrtCtrlType::kAck));

    uint32_t field0 = ntohl(*reinterpret_cast<const uint32_t*>(pkt.data()));
    // 高位置 1
    ASSERT_TRUE((field0 & 0x80000000u) != 0);
    // type 位于 bits16..30（大端 bytes 0..1 低 15 位）
    ASSERT_EQ((field0 >> 16) & 0x7FFFu, static_cast<uint32_t>(SrtCtrlType::kAck));
    // field0 低 16 位为保留位，必须为 0
    ASSERT_EQ(static_cast<uint16_t>(field0 & 0xFFFF), 0);
    // Additional Info 是 bytes 12..15 的完整 32 位大端字段
    uint32_t infoField = ntohl(
        *reinterpret_cast<const uint32_t*>(pkt.data() + 12));
    ASSERT_EQ(infoField, info);

    uint32_t dst = ntohl(*reinterpret_cast<const uint32_t*>(pkt.data() + 8));
    ASSERT_EQ(dst, destSock);
}

TEST(srt_control_types) {
    for (auto t : {SrtCtrlType::kHandshake, SrtCtrlType::kKeepAlive,
                   SrtCtrlType::kAck, SrtCtrlType::kNak,
                   SrtCtrlType::kShutdown, SrtCtrlType::kAckAck}) {
        auto pkt = srtBuildControl(t, 0, 0, 0);
        ASSERT_TRUE(srtIsControl(pkt.data()));
        ASSERT_EQ(srtCtrlType(pkt.data()), static_cast<uint16_t>(t));
    }
}

TEST(srt_handshake_constants) {
    ASSERT_EQ(static_cast<uint32_t>(SrtHandshakeType::kCaller), 1u);
    ASSERT_EQ(static_cast<uint32_t>(SrtHandshakeType::kResponder), 2u);
    ASSERT_EQ(static_cast<uint32_t>(SrtHandshakeType::kDone), 0xFFFFFFFFu);
    ASSERT_EQ(kSrtExtMagic, 0x4A52u);
}
