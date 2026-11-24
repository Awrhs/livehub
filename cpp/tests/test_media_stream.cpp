#include "rtmp/MediaStream.hpp"
#include "core/Buffer.hpp"
#include "protocol/FlvMuxer.hpp"
#include "test_framework.hpp"

#include <memory>
#include <string>
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

TEST(media_stream_assigns_unique_increasing_packet_seq) {
    MediaStream ms("k");
    std::vector<uint64_t> seqs;
    ms.subscribe([&](const MediaPacket& p) { seqs.push_back(p.seq); });
    ms.onPacket(pkt(MediaPacket::kVideo, 0, true));
    ms.onPacket(pkt(MediaPacket::kAudio, 10));
    ms.onPacket(pkt(MediaPacket::kVideo, 40));
    ASSERT_EQ(seqs.size(), static_cast<size_t>(3));
    ASSERT_TRUE(seqs[0] != 0);
    ASSERT_TRUE(seqs[1] > seqs[0]);
    ASSERT_TRUE(seqs[2] > seqs[1]);
}

TEST(flv_shared_tag_serialized_once_per_packet_for_all_viewers) {
    MediaStream ms("k");
    std::vector<std::shared_ptr<const std::string>> tags;
    // 两个订阅者相当于两个 FLV 观众：同一个包应拿到同一份缓冲
    ms.subscribe([&](const MediaPacket& p) { tags.push_back(FlvMuxer::sharedMediaTag(p)); });
    ms.subscribe([&](const MediaPacket& p) { tags.push_back(FlvMuxer::sharedMediaTag(p)); });
    ms.onPacket(pkt(MediaPacket::kVideo, 0, true));
    ms.onPacket(pkt(MediaPacket::kVideo, 40));
    ASSERT_EQ(tags.size(), static_cast<size_t>(4));
    ASSERT_TRUE(tags[0].get() == tags[1].get());
    ASSERT_TRUE(tags[2].get() == tags[3].get());
    ASSERT_TRUE(tags[0].get() != tags[2].get());

    // 内容与逐个序列化完全一致
    Buffer b;
    FlvMuxer::writeMediaTag(b, pkt(MediaPacket::kVideo, 40));
    ASSERT_TRUE(*tags[2] == b.retrieveAllAsString());

    // 新观众回放 GOP：GOP 中的包沿用原序号，结果与直播分发时一致
    std::vector<std::string> replay;
    ms.subscribeWithGop([&](const MediaPacket& p) {
        if (!p.isSequenceHeader) replay.push_back(*FlvMuxer::sharedMediaTag(p));
    });
    ASSERT_EQ(replay.size(), static_cast<size_t>(2));
    ASSERT_TRUE(replay[0] == *tags[0]);
    ASSERT_TRUE(replay[1] == *tags[2]);
}

TEST(flv_shared_tag_not_cached_without_seq_or_after_change) {
    MediaPacket p = pkt(MediaPacket::kAudio, 10);  // 未经 MediaStream 分发：seq = 0
    auto a = FlvMuxer::sharedMediaTag(p);
    auto b = FlvMuxer::sharedMediaTag(p);
    ASSERT_TRUE(a.get() != b.get());
    ASSERT_TRUE(*a == *b);

    p.seq = 987654321;
    auto c = FlvMuxer::sharedMediaTag(p);
    ASSERT_TRUE(c.get() == FlvMuxer::sharedMediaTag(p).get());
    p.timestamp = 20;  // 同序号但内容变了（防御）：不得复用旧结果
    auto d = FlvMuxer::sharedMediaTag(p);
    ASSERT_TRUE(d.get() != c.get());
    Buffer e;
    FlvMuxer::writeMediaTag(e, p);
    ASSERT_TRUE(*d == e.retrieveAllAsString());
}
