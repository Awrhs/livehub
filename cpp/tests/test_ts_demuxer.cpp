#include "hls/TsDemuxer.hpp"
#include "hls/TsMuxer.hpp"
#include "rtmp/AacParser.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <vector>

using namespace streaming;

// TsMuxer（已由 HLS E2E 验证）→ TsDemuxer 往返：应还原 AVC 序列头、关键帧与 AAC
TEST(ts_demuxer_roundtrip_avc_aac) {
    std::vector<uint8_t> ts;
    TsMuxer mux([&](const uint8_t* p, size_t n) { ts.insert(ts.end(), p, p + n); });

    const uint8_t sps[] = {0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00, 0xA0, 0x47, 0xFE, 0xC8};
    const uint8_t pps[] = {0x68, 0xCE, 0x3C, 0x80};
    std::vector<uint8_t> idr = {0x65, 0x88, 0x84, 0x00, 0x33, 0xFF};
    idr.resize(3000, 0x5A);
    std::vector<uint8_t> p = {0x41, 0x9A, 0x22, 0x10};
    p.resize(800, 0x3C);
    auto annexb = [](std::initializer_list<std::pair<const uint8_t*, size_t>> nals) {
        std::vector<uint8_t> out = {0, 0, 0, 1, 0x09, 0xF0};
        for (auto& [d, n] : nals) {
            out.insert(out.end(), {0, 0, 0, 1});
            out.insert(out.end(), d, d + n);
        }
        return out;
    };
    AacParser::Config cfg{};
    cfg.valid = true;
    cfg.profile = 1;
    cfg.sampleRateIndex = 4;
    cfg.channelConfig = 2;
    std::vector<uint8_t> raw(200, 0x21);

    for (int f = 0; f < 10; ++f) {
        int64_t t = f * 3600;  // 25fps @ 90kHz
        bool key = (f % 5 == 0);
        auto au = key ? annexb({{sps, sizeof(sps)}, {pps, sizeof(pps)}, {idr.data(), idr.size()}})
                      : annexb({{p.data(), p.size()}});
        mux.writeVideo(au.data(), au.size(), t, t, key);
        auto adts = AacParser::wrapWithAdts(cfg, raw.data(), raw.size());
        mux.writeAudio(adts.data(), adts.size(), t);
    }
    ASSERT_TRUE(ts.size() > 188 * 10);
    ASSERT_EQ(ts.size() % 188, size_t(0));

    TsDemuxer demux;
    int vSeq = 0, vKey = 0, vFrames = 0, aSeq = 0, aFrames = 0;
    demux.setPacketCallback([&](const MediaPacket& pkt) {
        if (pkt.type == MediaPacket::kVideo) {
            if (pkt.isSequenceHeader) ++vSeq;
            else {
                ++vFrames;
                if (pkt.isKeyframe) ++vKey;
            }
        } else if (pkt.type == MediaPacket::kAudio) {
            if (pkt.isSequenceHeader) ++aSeq;
            else ++aFrames;
        }
    });
    // 按 SRT 消息粒度（1316 字节）喂入
    for (size_t off = 0; off < ts.size(); off += 1316)
        demux.feed(ts.data() + off, std::min<size_t>(1316, ts.size() - off));

    ASSERT_TRUE(vSeq >= 1);
    ASSERT_TRUE(vKey >= 1);
    ASSERT_TRUE(vFrames >= 8);  // 最后一帧可能因无后续 PES 而未刷出
    ASSERT_TRUE(aSeq >= 1);
    ASSERT_TRUE(aFrames >= 8);
}
