#include "hls/PesPacket.hpp"
#include "test_framework.hpp"

using namespace streaming;

TEST(pes_pack_pts_only) {
    const uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    auto out = PesPacket::pack(0xE0, 90000, -1, payload, sizeof(payload));
    // start code 00 00 01 + stream id
    ASSERT_EQ(out[0], uint8_t(0x00));
    ASSERT_EQ(out[1], uint8_t(0x00));
    ASSERT_EQ(out[2], uint8_t(0x01));
    ASSERT_EQ(out[3], uint8_t(0xE0));
    // PTS marker (0x80) for PTS-only
    ASSERT_TRUE(out.size() >= 9);
}

TEST(pes_pack_pts_dts) {
    const uint8_t payload[] = {0x01};
    auto out = PesPacket::pack(0xE0, 180000, 90000, payload, 1);
    // PTS+DTS marker is 0xC0
    ASSERT_TRUE(out.size() >= 14);
    // PTS DTS flags 位在 byte7
    ASSERT_EQ(out[7] & 0xC0, uint8_t(0xC0));
}
