#include "rtmp/MediaStream.hpp"
#include "rtmp/EnhancedRtmp.hpp"
#include "rtmp/AacParser.hpp"
#include "rtmp/AvcParser.hpp"
#include "rtmp/HevcParser.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <vector>

using namespace streaming;

// ---- HEVC ----

TEST(hevc_nalu_type) {
    // 2 字节 NALU header：nal_unit_type 在第 0 字节高 6 位 >> 1
    // VPS type=32 -> header[0] = 32<<1 = 64
    uint8_t vps[] = {0x40, 0x01, 0x00, 0x00};
    ASSERT_EQ(HevcParser::naluType(vps, sizeof(vps)), 32);
    // SPS type=33
    uint8_t sps[] = {0x42, 0x01, 0x00};
    ASSERT_EQ(HevcParser::naluType(sps, sizeof(sps)), 33);
    // IDR_W_RADL type=19
    uint8_t idr[] = {0x26, 0x01, 0x00};
    ASSERT_EQ(HevcParser::naluType(idr, sizeof(idr)), 19);
}

TEST(hevc_keyframe_classification) {
    ASSERT_TRUE(HevcParser::isKeyframeNalu(HevcParser::kNalIdrWRadl));
    ASSERT_TRUE(HevcParser::isKeyframeNalu(HevcParser::kNalIdrNLp));
    ASSERT_TRUE(HevcParser::isKeyframeNalu(HevcParser::kNalCraNut));
    ASSERT_FALSE(HevcParser::isKeyframeNalu(HevcParser::kNalVps));
    // Annex-B 起始码 + IDR
    uint8_t au[] = {0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x02};
    ASSERT_TRUE(HevcParser::isKeyframe(au, sizeof(au)));
}

TEST(hevc_hvcc_roundtrip) {
    // 构造最小（内容任意）的 VPS/SPS/PPS，buildHvccFromAnnexB 应当能
    // 生成可被 extractConfig 还原的 HEVCDecoderConfigurationRecord。
    std::vector<uint8_t> vps = {0x40, 0x01, 0xAA};
    std::vector<uint8_t> sps = {0x42, 0x01, 0xBB, 0xCC};
    std::vector<uint8_t> pps = {0x44, 0x01, 0xDD};
    auto hvcc = HevcParser::buildHvccFromAnnexB(vps, sps, pps);

    ASSERT_GT(hvcc.size(), size_t(23));
    ASSERT_EQ(hvcc[0], 1);  // configurationVersion
    ASSERT_EQ(HevcParser::lengthSizeMinusOne(hvcc.data(), hvcc.size()), 3);

    std::vector<uint8_t> vps2, sps2, pps2;
    ASSERT_TRUE(HevcParser::extractConfig(hvcc.data(), hvcc.size(),
                                         vps2, sps2, pps2));
    // extractConfig 会给每个 NALU 加 4 字节 Annex-B 起始码
    auto withStart = [](const std::vector<uint8_t>& n) {
        std::vector<uint8_t> v = {0, 0, 0, 1};
        v.insert(v.end(), n.begin(), n.end());
        return v;
    };
    ASSERT_EQ(vps2, withStart(vps));
    ASSERT_EQ(sps2, withStart(sps));
    ASSERT_EQ(pps2, withStart(pps));
}

TEST(hevc_hvcc_to_annexb) {
    std::vector<uint8_t> vps = {0x40, 0x01};
    std::vector<uint8_t> sps = {0x42, 0x01};
    std::vector<uint8_t> pps = {0x44, 0x01};
    auto hvcc = HevcParser::buildHvccFromAnnexB(vps, sps, pps);

    auto annexB = HevcParser::buildAnnexBConfig(hvcc.data(), hvcc.size());
    // 三个起始码
    int starts = 0;
    for (size_t i = 0; i + 3 < annexB.size(); ++i) {
        if (annexB[i] == 0 && annexB[i + 1] == 0 && annexB[i + 2] == 0 &&
            annexB[i + 3] == 1)
            ++starts;
    }
    ASSERT_EQ(starts, 3);
}

// ---- AAC ----

TEST(aac_adts_wrap) {
    AacParser::Config cfg;
    cfg.profile = 1;        // LC
    cfg.sampleRateIndex = 4; // 44100
    cfg.channelConfig = 2;
    cfg.valid = true;

    uint8_t frame[] = {0x21, 0x10, 0x05, 0x60};
    auto out = AacParser::wrapWithAdts(cfg, frame, sizeof(frame));

    // 7 字节 ADTS 头 + 4 字节帧
    ASSERT_EQ(out.size(), size_t(11));
    // syncword 0xFFF
    ASSERT_EQ(out[0], 0xFF);
    ASSERT_EQ(out[1] & 0xF0, 0xF0);
    // profile = objectType-1 = 0 (LC)，在 byte2 的高 2 位
    ASSERT_EQ((out[2] >> 6) & 0x03, 0);
    // frame length 字段（13 位）应为 11
    size_t flen = ((static_cast<size_t>(out[3] & 0x03)) << 11) |
                  (static_cast<size_t>(out[4]) << 3) |
                  (out[5] >> 5);
    ASSERT_EQ(flen, size_t(11));
    // 原始帧内容跟在 ADTS 头之后
    for (size_t i = 0; i < sizeof(frame); ++i)
        ASSERT_EQ(out[7 + i], frame[i]);
}

TEST(aac_sample_rate_index) {
    ASSERT_EQ(AacParser::sampleRateFromIndex(4), 44100);
    ASSERT_EQ(AacParser::sampleRateFromIndex(3), 48000);
    ASSERT_EQ(AacParser::sampleRateFromIndex(0), 96000);
}

// ---- AVC ----

TEST(avc_keyframe_and_seqhdr) {
    // AVC sequence header：byte0=0x17 keyframe, byte1=0x00 seq header
    uint8_t seq[] = {0x17, 0x00, 0x00, 0x00, 0x00};
    ASSERT_TRUE(AvcParser::isKeyframe(seq, sizeof(seq)));
    ASSERT_TRUE(AvcParser::isSequenceHeader(seq, sizeof(seq)));
    // NALU type：SPS=7，header byte0 的低 5 位
    uint8_t spsNalu[] = {0x67, 0x42, 0x00};
    ASSERT_EQ(AvcParser::naluType(spsNalu, sizeof(spsNalu)), 7);
    // Coded slice of IDR = 5
    uint8_t idrNalu[] = {0x65, 0x88};
    ASSERT_EQ(AvcParser::naluType(idrNalu, sizeof(idrNalu)), 5);
}

TEST(avc_avcC_roundtrip) {
    // 最小的 AVC SPS/PPS（内容任意，结构字段由 buildAvcC 写入）
    std::vector<uint8_t> sps = {0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00};
    std::vector<uint8_t> pps = {0x68, 0xCE, 0x38, 0x80};
    auto avcc = AvcParser::buildAvcC(sps, pps);

    // 6 字节头 + 2(sps len) + sps + 1(numPps) + 2(pps len) + pps
    ASSERT_EQ(avcc.size(), size_t(6 + 2 + sps.size() + 1 + 2 + pps.size()));
    ASSERT_EQ(avcc[0], 1);  // version
    ASSERT_EQ(avcc[1], sps[1]);  // profile
    ASSERT_EQ(avcc[4], 0xFF);   // lengthSizeMinusOne = 3
    ASSERT_EQ(avcc[5], 0xE1);   // numOfSPS = 1
    ASSERT_EQ(avcc[8 + sps.size()], 1);  // numOfPPS

    std::vector<uint8_t> spsOut, ppsOut;
    // extractSpsPps 输出会带 4 字节 Annex-B 起始码
    ASSERT_TRUE(AvcParser::extractSpsPps(avcc.data(), avcc.size(),
                                        spsOut, ppsOut));
    std::vector<uint8_t> expSps = {0, 0, 0, 1};
    expSps.insert(expSps.end(), sps.begin(), sps.end());
    std::vector<uint8_t> expPps = {0, 0, 0, 1};
    expPps.insert(expPps.end(), pps.begin(), pps.end());
    ASSERT_EQ(spsOut, expSps);
    ASSERT_EQ(ppsOut, expPps);
}

// Enhanced RTMP：按规范字节布局（与 FFmpeg flvenc 输出一致）
TEST(enhanced_rtmp_video_parse_spec_layout) {
    // 关键帧 + SequenceStart：0x80 | (1<<4) | 0 = 0x90，'hvc1'，后接 hvcC
    const uint8_t seq[] = {0x90, 'h', 'v', 'c', '1', 0x01, 0x02, 0x03};
    EnhancedRtmp::VideoInfo vi;
    ASSERT_TRUE(EnhancedRtmp::isExVideo(seq[0]));
    ASSERT_TRUE(EnhancedRtmp::parseVideo(seq, sizeof(seq), vi));
    ASSERT_EQ(vi.frameType, 1);
    ASSERT_EQ(vi.packetType, uint8_t(EnhancedRtmp::kPktSequenceStart));
    ASSERT_TRUE(vi.codec == VideoCodec::kHevc);
    ASSERT_EQ(vi.payloadLen, size_t(3));
    ASSERT_EQ(vi.payload[0], uint8_t(0x01));

    // 帧间 + CodedFrames：0x80 | (2<<4) | 1 = 0xA1，cts = -2
    const uint8_t cf[] = {0xA1, 'h', 'v', 'c', '1', 0xFF, 0xFF, 0xFE, 0xAA};
    ASSERT_TRUE(EnhancedRtmp::parseVideo(cf, sizeof(cf), vi));
    ASSERT_EQ(vi.frameType, 2);
    ASSERT_EQ(vi.cts, -2);
    ASSERT_EQ(vi.payloadLen, size_t(1));
    ASSERT_EQ(vi.payload[0], uint8_t(0xAA));

    // 关键帧 + CodedFramesX（无 CTS）：0x93
    const uint8_t cfx[] = {0x93, 'h', 'v', 'c', '1', 0xBB, 0xCC};
    ASSERT_TRUE(EnhancedRtmp::parseVideo(cfx, sizeof(cfx), vi));
    ASSERT_EQ(vi.packetType, uint8_t(EnhancedRtmp::kPktCodedFramesX));
    ASSERT_EQ(vi.cts, 0);
    ASSERT_EQ(vi.payloadLen, size_t(2));

    // legacy AVC 不是扩展头
    ASSERT_FALSE(EnhancedRtmp::isExVideo(0x17));
    ASSERT_FALSE(EnhancedRtmp::isExVideo(0x27));
}

TEST(enhanced_rtmp_build_roundtrip) {
    const uint8_t data[] = {9, 8, 7};
    auto msg = EnhancedRtmp::buildVideoMessage(EnhancedRtmp::kFourccHvc1, 1,
                                               EnhancedRtmp::kPktCodedFrames, 40,
                                               data, sizeof(data));
    ASSERT_EQ(msg[0], uint8_t(0x91));
    EnhancedRtmp::VideoInfo vi;
    ASSERT_TRUE(EnhancedRtmp::parseVideo(msg.data(), msg.size(), vi));
    ASSERT_EQ(vi.cts, 40);
    ASSERT_EQ(vi.payloadLen, size_t(3));
    ASSERT_EQ(vi.payload[2], uint8_t(7));

    auto am = EnhancedRtmp::buildAudioMessage(EnhancedRtmp::kFourccOpus, 1, data, 3);
    ASSERT_EQ(am[0], uint8_t(0x91));
    EnhancedRtmp::AudioInfo ai;
    ASSERT_TRUE(EnhancedRtmp::parseAudio(am.data(), am.size(), ai));
    ASSERT_EQ(ai.packetType, uint8_t(1));
    ASSERT_EQ(ai.payloadLen, size_t(3));
    // legacy AAC（0xAF）不是扩展头
    ASSERT_FALSE(EnhancedRtmp::isExAudio(0xAF));
}
