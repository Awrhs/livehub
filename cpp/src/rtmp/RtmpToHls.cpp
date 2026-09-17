#include "rtmp/RtmpToHls.hpp"

#include "core/Logger.hpp"
#include "hls/HlsManager.hpp"
#include "hls/HlsSource.hpp"
#include "hls/TsPacket.hpp"
#include "rtmp/EnhancedRtmp.hpp"
#include "rtmp/HevcParser.hpp"

namespace streaming {

RtmpToHls::RtmpToHls(std::shared_ptr<MediaStream> stream, HlsManager* manager,
                     std::string outputDir, int targetDuration,
                     int windowSize)
    : stream_(std::move(stream)),
      manager_(manager),
      outputDir_(std::move(outputDir)),
      targetDuration_(targetDuration),
      windowSize_(windowSize) {}

RtmpToHls::~RtmpToHls() {
    stop();
}

void RtmpToHls::start() {
    if (manager_) {
        hls_ = manager_->getOrCreate(stream_->key());
    } else {
        hls_ = std::make_shared<HlsSource>(stream_->key(), outputDir_,
                                           targetDuration_, windowSize_);
        hls_->start();
    }
    subId_ = stream_->subscribe(
        [this](const MediaPacket& p) { onPacket(p); });
    LOG_INFO("RtmpToHls bridge started for stream=%s", stream_->key().c_str());
}

void RtmpToHls::stop() {
    if (subId_ != 0) {
        stream_->unsubscribe(subId_);
        subId_ = 0;
    }
}

void RtmpToHls::onPacket(const MediaPacket& pkt) {
    if (!hls_) return;
    switch (pkt.type) {
        case MediaPacket::kVideo:
            onVideo(pkt);
            break;
        case MediaPacket::kAudio:
            onAudio(pkt);
            break;
        case MediaPacket::kScript:
            break;
    }
}

void RtmpToHls::onVideo(const MediaPacket& pkt) {
    if (pkt.data.size() < 5) return;
    const uint8_t* d = pkt.data.data();
    size_t len = pkt.data.size();

    // ---- Enhanced RTMP (H.265/HEVC 等) ----
    if (pkt.enhanced) {
        EnhancedRtmp::VideoInfo vi;
        if (!EnhancedRtmp::parseVideo(d, len, vi)) return;
        if (vi.codec == VideoCodec::kHevc) {
            videoCodec_ = VideoCodec::kHevc;
            if (hls_) hls_->setVideoStreamType(kStreamTypeHevc);
            if (vi.packetType == 0 || vi.packetType == 3) {
                // HEVCDecoderConfigurationRecord
                std::vector<uint8_t> vps, sps, pps;
                if (HevcParser::extractConfig(vi.payload, vi.payloadLen, vps,
                                             sps, pps)) {
                    vps_ = std::move(vps);
                    sps_ = std::move(sps);
                    pps_ = std::move(pps);
                    hasVideoConfig_ = true;
                    int64_t t90 = static_cast<int64_t>(pkt.timestamp) * 90;
                    if (!vps_.empty())
                        hls_->onVideoFrame(vps_.data(), vps_.size(), t90, t90,
                                           false);
                    if (!sps_.empty())
                        hls_->onVideoFrame(sps_.data(), sps_.size(), t90, t90,
                                           false);
                    if (!pps_.empty())
                        hls_->onVideoFrame(pps_.data(), pps_.size(), t90, t90,
                                           false);
                }
                return;
            }
            if (vi.packetType != 1 && vi.packetType != 2) return;
            if (vi.payloadLen == 0) return;
            std::vector<uint8_t> annexb =
                HevcParser::hvccToAnnexB(vi.payload, vi.payloadLen, 3);
            if (annexb.empty()) return;
            int64_t dts90k = static_cast<int64_t>(pkt.timestamp) * 90;
            int64_t pts90k = dts90k + static_cast<int64_t>(vi.cts) * 90;
            hls_->onVideoFrame(annexb.data(), annexb.size(), pts90k, dts90k,
                               pkt.isKeyframe);
        }
        // av01/vp09 暂不在 TS 中支持，忽略
        return;
    }

    // ---- Legacy AVC / H.264 ----
    int avcType = AvcParser::avcPacketType(d, len);
    if (avcType < 0) return;
    videoCodec_ = VideoCodec::kH264;
    if (hls_) hls_->setVideoStreamType(kStreamTypeVideo);

    if (avcType == 0) {
        std::vector<uint8_t> sps, pps;
        if (AvcParser::extractSpsPps(d, len, sps, pps)) {
            sps_ = std::move(sps);
            pps_ = std::move(pps);
            hasVideoConfig_ = true;
            if (!sps_.empty()) {
                hls_->onVideoFrame(sps_.data(), sps_.size(),
                                   pkt.timestamp * 90LL, pkt.timestamp * 90LL,
                                   true);
            }
            if (!pps_.empty()) {
                hls_->onVideoFrame(pps_.data(), pps_.size(),
                                   pkt.timestamp * 90LL, pkt.timestamp * 90LL,
                                   false);
            }
        }
        return;
    }
    if (avcType == 2) return;  // end of sequence

    int32_t cts = static_cast<int32_t>(
        (static_cast<uint32_t>(d[2]) << 16) |
        (static_cast<uint32_t>(d[3]) << 8) |
        static_cast<uint32_t>(d[4]));
    if (cts & 0x00800000) cts |= ~0x00FFFFFF;

    std::vector<uint8_t> annexb = AvcParser::avccToAnnexB(d + 5, len - 5);
    if (annexb.empty()) return;

    int64_t dts90k = static_cast<int64_t>(pkt.timestamp) * 90;
    int64_t pts90k = dts90k + static_cast<int64_t>(cts) * 90;
    hls_->onVideoFrame(annexb.data(), annexb.size(), pts90k, dts90k,
                       pkt.isKeyframe);
}

void RtmpToHls::onAudio(const MediaPacket& pkt) {
    if (pkt.data.size() < 2) return;
    const uint8_t* d = pkt.data.data();
    size_t len = pkt.data.size();

    if (AacParser::isSequenceHeader(d, len)) {
        if (len >= 4) {
            aacCfg_ = AacParser::parseAudioSpecificConfig(d + 2, len - 2);
            hasAudioConfig_ = aacCfg_.valid;
        }
        return;
    }
    if (!hasAudioConfig_) return;

    // AACPacketType==1：byte0 是音频头，byte1 起为 raw AAC frame
    std::vector<uint8_t> adts =
        AacParser::wrapWithAdts(aacCfg_, d + 1, len - 1);
    hls_->onAudioFrame(adts.data(), adts.size(),
                       static_cast<int64_t>(pkt.timestamp) * 90);
}

}  // namespace streaming
