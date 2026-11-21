#include "hls/FlvToTs.hpp"

#include "hls/TsPacket.hpp"
#include "rtmp/AvcParser.hpp"
#include "rtmp/EnhancedRtmp.hpp"
#include "rtmp/HevcParser.hpp"

namespace streaming {

namespace {
const uint8_t kAvcAud[] = {0, 0, 0, 1, 0x09, 0xF0};
const uint8_t kHevcAud[] = {0, 0, 0, 1, 0x46, 0x01, 0x50};
}  // namespace

void FlvToTs::onPacket(const MediaPacket& pkt) {
    if (pkt.type == MediaPacket::kVideo) onVideo(pkt);
    else if (pkt.type == MediaPacket::kAudio) onAudio(pkt);
}

void FlvToTs::onVideo(const MediaPacket& pkt) {
    const uint8_t* d = pkt.data.data();
    size_t n = pkt.data.size();
    if (n < 2) return;

    const uint8_t* nal = nullptr;
    size_t nalLen = 0;
    int32_t cts = 0;

    if (EnhancedRtmp::isExVideo(d[0])) {
        EnhancedRtmp::VideoInfo vi;
        if (!EnhancedRtmp::parseVideo(d, n, vi) || vi.codec != VideoCodec::kHevc) return;
        if (vi.packetType == EnhancedRtmp::kPktSequenceStart) {
            std::vector<uint8_t> v, s, p;
            if (HevcParser::extractConfig(vi.payload, vi.payloadLen, v, s, p)) {
                vps_ = std::move(v);
                sps_ = std::move(s);
                pps_ = std::move(p);
                hevc_ = true;
                haveConfig_ = true;
                muxer_.setVideoStreamType(kStreamTypeHevc);
            }
            return;
        }
        if (vi.packetType != EnhancedRtmp::kPktCodedFrames &&
            vi.packetType != EnhancedRtmp::kPktCodedFramesX)
            return;
        nal = vi.payload;
        nalLen = vi.payloadLen;
        cts = vi.cts;
    } else {
        int t = AvcParser::avcPacketType(d, n);
        if (t == 0) {
            std::vector<uint8_t> s, p;
            if (n > 5 && AvcParser::extractSpsPps(d + 5, n - 5, s, p)) {
                sps_ = std::move(s);
                pps_ = std::move(p);
                hevc_ = false;
                haveConfig_ = true;
                muxer_.setVideoStreamType(kStreamTypeVideo);
            }
            return;
        }
        if (t != 1 || n <= 5) return;
        cts = (int32_t(d[2]) << 16) | (int32_t(d[3]) << 8) | int32_t(d[4]);
        if (cts & 0x800000) cts |= ~0x00FFFFFF;
        nal = d + 5;
        nalLen = n - 5;
    }
    if (!haveConfig_) return;
    if (!started_) {
        if (!pkt.isKeyframe) return;
        started_ = true;
    }

    std::vector<uint8_t> frame = hevc_ ? HevcParser::hvccToAnnexB(nal, nalLen, 3)
                                       : AvcParser::avccToAnnexB(nal, nalLen);
    if (frame.empty()) return;
    std::vector<uint8_t> au;
    au.reserve(frame.size() + 128);
    if (hevc_) au.insert(au.end(), kHevcAud, kHevcAud + sizeof(kHevcAud));
    else au.insert(au.end(), kAvcAud, kAvcAud + sizeof(kAvcAud));
    if (pkt.isKeyframe) {
        au.insert(au.end(), vps_.begin(), vps_.end());
        au.insert(au.end(), sps_.begin(), sps_.end());
        au.insert(au.end(), pps_.begin(), pps_.end());
    }
    au.insert(au.end(), frame.begin(), frame.end());

    int64_t dts90 = static_cast<int64_t>(pkt.timestamp) * 90;
    int64_t pts90 = dts90 + static_cast<int64_t>(cts) * 90;
    muxer_.writeVideo(au.data(), au.size(), pts90, dts90, pkt.isKeyframe);
}

void FlvToTs::onAudio(const MediaPacket& pkt) {
    const uint8_t* d = pkt.data.data();
    size_t n = pkt.data.size();
    if (n < 2 || (d[0] >> 4) != 10) return;  // 仅 legacy AAC
    if (d[1] == 0) {
        aac_ = AacParser::parseAudioSpecificConfig(d + 2, n - 2);
        haveAac_ = aac_.valid;
        return;
    }
    if (!haveAac_ || n <= 2) return;
    // 有视频时，首个视频关键帧之前的音频丢弃，保证从可解码位置开始
    if (haveConfig_ && !started_) return;
    auto adts = AacParser::wrapWithAdts(aac_, d + 2, n - 2);
    muxer_.writeAudio(adts.data(), adts.size(),
                      static_cast<int64_t>(pkt.timestamp) * 90);
}

}  // namespace streaming
