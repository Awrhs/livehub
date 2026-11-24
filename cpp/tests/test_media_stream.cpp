#include "rtmp/MediaStream.hpp"
#include "test_framework.hpp"

#include <vector>

using namespace streaming;

namespace {
MediaPacket pkt(MediaPacket::Type type, uint32_t ts, bool key = false) {
    MediaPacket p;
    p.type = type;
    p.timestamp = ts;
    p.isKeyframe = key;
    p.data = {0x17, 0x01, 0, 0, 0};
    return p;
}

std::vector<uint32_t> feed(MediaStream& ms, const std::vector<MediaPacket>& in) {
    std::vector<uint32_t> out;
    ms.subscribe([&](const MediaPacket& p) { out.push_back(p.timestamp); });
    for (const auto& p : in) ms.onPacket(p);
    return out;
}
}  // namespace

TEST(media_stream_timestamps_pass_through_when_continuous) {
    MediaStream ms("k");
    auto out = feed(ms, {pkt(MediaPacket::kVideo, 1000, true), pkt(MediaPacket::kVideo, 1040),
                         pkt(MediaPacket::kAudio, 1020), pkt(MediaPacket::kVideo, 1080)});
    // 正常的音视频交织（音频略晚于视频）保持原样
    ASSERT_EQ(out.size(), 4u);
    ASSERT_EQ(out[0], 1000u);
    ASSERT_EQ(out[1], 1040u);
    ASSERT_EQ(out[2], 1020u);
    ASSERT_EQ(out[3], 1080u);
    ASSERT_EQ(ms.timestampJumps(), 0u);
}

TEST(media_stream_timestamps_backward_jump_is_made_continuous) {
    MediaStream ms("k");
    // 编码器重启：时间戳从 60000 回到 0
    auto out = feed(ms, {pkt(MediaPacket::kVideo, 60000, true), pkt(MediaPacket::kVideo, 60040),
                         pkt(MediaPacket::kVideo, 0, true), pkt(MediaPacket::kVideo, 40),
                         pkt(MediaPacket::kAudio, 30)});
    ASSERT_EQ(out[2], 60080u);  // 接在上一包之后
    ASSERT_EQ(out[3], 60120u);  // 后续按同一偏移平移
    ASSERT_EQ(out[4], 60110u);
    ASSERT_EQ(ms.timestampJumps(), 1u);
}

TEST(media_stream_timestamps_forward_jump_is_made_continuous) {
    MediaStream ms("k");
    auto out = feed(ms, {pkt(MediaPacket::kVideo, 0, true), pkt(MediaPacket::kVideo, 40),
                         pkt(MediaPacket::kVideo, 3600000), pkt(MediaPacket::kVideo, 3600040)});
    ASSERT_EQ(out[2], 80u);
    ASSERT_EQ(out[3], 120u);
    ASSERT_EQ(ms.timestampJumps(), 1u);
}

TEST(media_stream_timestamps_32bit_wraparound) {
    MediaStream ms("k");
    auto out = feed(ms, {pkt(MediaPacket::kVideo, 0xFFFFFF00u, true), pkt(MediaPacket::kVideo, 0xFFFFFFF0u),
                         pkt(MediaPacket::kVideo, 0x00000010u)});
    // 回绕被当作一次跳变处理，输出连续（按 32 位取模）
    ASSERT_EQ(out[2], static_cast<uint32_t>(0xFFFFFFF0u + 40u));
}
