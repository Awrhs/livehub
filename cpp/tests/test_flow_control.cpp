#include "rtmp/FlowControl.hpp"
#include "test_framework.hpp"

using namespace streaming;

namespace {
MediaPacket vid(bool key) {
    MediaPacket p;
    p.type = MediaPacket::kVideo;
    p.timestamp = 0;
    p.isKeyframe = key;
    return p;
}
MediaPacket aud() {
    MediaPacket p;
    p.type = MediaPacket::kAudio;
    p.timestamp = 0;
    return p;
}
}  // namespace

TEST(flow_control_normal_send) {
    FlowControl fc(1000, 5000);
    ASSERT_EQ(fc.check(vid(true), 0), FlowControl::kSend);
    ASSERT_EQ(fc.check(aud(), 999), FlowControl::kSend);
    ASSERT_FALSE(fc.dropping());
}

TEST(flow_control_drop_until_keyframe_and_drain) {
    FlowControl fc(1000, 5000);
    ASSERT_EQ(fc.check(vid(false), 1500), FlowControl::kDrop);
    ASSERT_TRUE(fc.dropping());
    // 积压回落但不是关键帧：继续丢
    ASSERT_EQ(fc.check(vid(false), 100), FlowControl::kDrop);
    ASSERT_EQ(fc.check(aud(), 100), FlowControl::kDrop);
    // 关键帧但积压仍高于 soft/2：继续丢
    ASSERT_EQ(fc.check(vid(true), 600), FlowControl::kDrop);
    // 关键帧且积压已排空：恢复
    ASSERT_EQ(fc.check(vid(true), 400), FlowControl::kSend);
    ASSERT_FALSE(fc.dropping());
    ASSERT_EQ(fc.dropped(), uint64_t(4));
}

TEST(flow_control_seq_header_never_dropped) {
    FlowControl fc(1000, 5000);
    fc.check(vid(false), 2000);  // 进入丢帧
    MediaPacket sh = vid(true);
    sh.isSequenceHeader = true;
    ASSERT_EQ(fc.check(sh, 2000), FlowControl::kSend);
}

TEST(flow_control_hard_limit_closes) {
    FlowControl fc(1000, 5000);
    ASSERT_EQ(fc.check(vid(true), 5000), FlowControl::kClose);
    MediaPacket sh = vid(true);
    sh.isSequenceHeader = true;
    ASSERT_EQ(fc.check(sh, 6000), FlowControl::kClose);
}
