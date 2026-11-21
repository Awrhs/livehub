#include "hls/TsPacket.hpp"
#include "test_framework.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace streaming;

TEST(ts_packet_size_and_sync) {
    std::array<uint8_t, 188> out{};
    size_t n = TsPacket::buildPat(out.data(), 0x1000, 0);
    ASSERT_EQ(n, size_t(188));
    ASSERT_EQ(out[0], uint8_t(0x47));
}

TEST(ts_continuity_counter_increments) {
    std::array<uint8_t, 188> a{}, b{};
    TsPacket::buildPat(a.data(), 0x1000, 0);
    TsPacket::buildPat(b.data(), 0x1000, 1);
    // CC 在 byte3 的低 4 位
    ASSERT_EQ(a[3] & 0x0F, uint8_t(0));
    ASSERT_EQ(b[3] & 0x0F, uint8_t(1));
}

TEST(ts_pmt_pids) {
    std::array<uint8_t, 188> out{};
    size_t n = TsPacket::buildPmt(out.data(), 0x1000, 0x0100, 0x0100, 0x0101, 0);
    ASSERT_EQ(n, size_t(188));
    ASSERT_EQ(out[0], uint8_t(0x47));
}

TEST(ts_pes_carries_payload) {
    std::array<uint8_t, 188> out{};
    const uint8_t payload[] = {0x00, 0x01, 0x02, 0x03};
    size_t consumed = 0;
    size_t n = TsPacket::buildPes(out.data(), 0x0100, 0, true, payload,
                                   sizeof(payload), -1, false, &consumed);
    ASSERT_EQ(n, size_t(188));
    ASSERT_GT(consumed, size_t(0));
}

TEST(ts_crc32_deterministic) {
    const char* s = "123456789";
    uint32_t c1 = TsPacket::crc32(reinterpret_cast<const uint8_t*>(s), 9);
    uint32_t c2 = TsPacket::crc32(reinterpret_cast<const uint8_t*>(s), 9);
    ASSERT_EQ(c1, c2);
    ASSERT_NE(c1, uint32_t(0));
}

// 回归：PID 编码不得污染第 12 位（原实现所有 PID +0x1000，PAT 不在 PID 0）
TEST(ts_header_pid_encoding) {
    uint8_t pkt[188];
    TsPacket::buildPat(pkt, 0x1000, 0);
    ASSERT_EQ(pkt[0], uint8_t(0x47));
    uint16_t pid = static_cast<uint16_t>(((pkt[1] & 0x1F) << 8) | pkt[2]);
    ASSERT_EQ(pid, uint16_t(0));
    ASSERT_TRUE((pkt[1] & 0x40) != 0);  // PUSI

    uint8_t v = 0;
    size_t used = 0;
    TsPacket::buildPes(pkt, 0x0100, 3, false, &v, 1, -1, false, &used);
    pid = static_cast<uint16_t>(((pkt[1] & 0x1F) << 8) | pkt[2]);
    ASSERT_EQ(pid, uint16_t(0x0100));
}

// 回归：负载不足时必须用 adaptation field 填充，payload 必须恰好位于包尾
TEST(ts_pes_short_payload_uses_af_stuffing) {
    uint8_t pkt[188];
    const uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    size_t used = 0;
    TsPacket::buildPes(pkt, 0x101, 0, false, data, sizeof(data), -1, false, &used);
    ASSERT_EQ(used, size_t(10));
    ASSERT_EQ((pkt[3] >> 4) & 3, 3);  // adaptation + payload
    size_t afLen = pkt[4];
    ASSERT_EQ(4 + 1 + afLen + 10, size_t(188));
    ASSERT_TRUE(std::memcmp(pkt + 178, data, 10) == 0);

    // 恰好差 1 字节：adaptation_field_length = 0 的单字节 AF
    std::vector<uint8_t> big(183, 0xAB);
    TsPacket::buildPes(pkt, 0x101, 0, false, big.data(), big.size(), -1, false, &used);
    ASSERT_EQ(used, size_t(183));
    ASSERT_EQ(pkt[4], uint8_t(0));
    ASSERT_EQ(pkt[5], uint8_t(0xAB));
}

// PCR 编码：33 位 base 可正确还原
TEST(ts_pcr_encoding_roundtrip) {
    uint8_t pkt[188];
    const uint8_t d = 0;
    int64_t pcr = 0x1ABCDEF01LL;  // 33 位
    size_t used = 0;
    TsPacket::buildPes(pkt, 0x100, 0, true, &d, 1, pcr, true, &used);
    ASSERT_TRUE(pkt[5] & 0x10);  // PCR flag
    ASSERT_TRUE(pkt[5] & 0x40);  // random access
    const uint8_t* p = pkt + 6;
    uint64_t base = (uint64_t(p[0]) << 25) | (uint64_t(p[1]) << 17) |
                    (uint64_t(p[2]) << 9) | (uint64_t(p[3]) << 1) | (p[4] >> 7);
    ASSERT_EQ(base, uint64_t(pcr));
}
