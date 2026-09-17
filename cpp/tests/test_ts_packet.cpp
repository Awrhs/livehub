#include "hls/TsPacket.hpp"
#include "test_framework.hpp"

#include <array>
#include <cstring>

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
