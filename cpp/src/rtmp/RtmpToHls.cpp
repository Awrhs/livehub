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
    // 原子回放已有 seq header/GOP 后订阅（桥接晚于首包创建时也不会缺配置）
    subId_ = stream_->subscribeWithGop(
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
            if (vi.packetType == EnhancedRtmp::kPktSequenceStart) {
                // HEVCDecoderConfigurationRecord
                std::vector<uint8_t> vps, sps, pps;
                if (HevcParser::extractConfig(vi.payload, vi.payloadLen, vps,
                                             sps, pps)) {
                    // 仅保存；在每个关键帧前内联（见下）
                    vps_ = std::move(vps);
                    sps_ = std::move(sps);
                    pps_ = std::move(pps);
                    hasVideoConfig_ = true;
                }
                return;
            }
            if (vi.packetType != EnhancedRtmp::kPktCodedFrames &&
                vi.packetType != EnhancedRtmp::kPktCodedFramesX)
                return;
            if (vi.payloadLen == 0) return;
            std::vector<uint8_t> nalus =
                HevcParser::hvccToAnnexB(vi.payload, vi.payloadLen, 3);
            if (nalus.empty()) return;
            // AUD(type 35) + [VPS/SPS/PPS 于关键帧] + 帧数据
            static const uint8_t kHevcAud[] = {0, 0, 0, 1, 0x46, 0x01, 0x50};
            std::vector<uint8_t> au(kHevcAud, kHevcAud + sizeof(kHevcAud));
            if (pkt.isKeyframe) {
                au.insert(au.end(), vps_.begin(), vps_.end());
                au.insert(au.end(), sps_.begin(), sps_.end());
                au.insert(au.end(), pps_.begin(), pps_.end());
            }
            au.insert(au.end(), nalus.begin(), nalus.end());
            int64_t dts90k = static_cast<int64_t>(pkt.timestamp) * 90;
            int64_t pts90k = dts90k + static_cast<int64_t>(vi.cts) * 90;
            hls_->onVideoFrame(au.data(), au.size(), pts90k, dts90k,
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
        // AVCDecoderConfigurationRecord 从 RTMP 负载第 5 字节开始
        // （原实现传入整个负载，numSps 读错位置，SPS/PPS 从未提取成功）
        std::vector<uint8_t> sps, pps;
        if (len > 5 && AvcParser::extractSpsPps(d + 5, len - 5, sps, pps)) {
            sps_ = std::move(sps);
            pps_ = std::move(pps);
            hasVideoConfig_ = true;
        }
        return;  // 仅保存；在每个 IDR 前内联（TS 没有带外 extradata）
    }
    if (avcType == 2) return;  // end of sequence

    int32_t cts = static_cast<int32_t>(
        (static_cast<uint32_t>(d[2]) << 16) |
        (static_cast<uint32_t>(d[3]) << 8) |
        static_cast<uint32_t>(d[4]));
    if (cts & 0x00800000) cts |= ~0x00FFFFFF;

    std::vector<uint8_t> nalus = AvcParser::avccToAnnexB(d + 5, len - 5);
    if (nalus.empty()) return;

    // AUD(type 9) + [SPS + PPS 于关键帧] + 帧数据（Apple HLS 规范建议每个 AU 带 AUD）
    static const uint8_t kAvcAud[] = {0, 0, 0, 1, 0x09, 0xF0};
    std::vector<uint8_t> au(kAvcAud, kAvcAud + sizeof(kAvcAud));
    if (pkt.isKeyframe) {
        au.insert(au.end(), sps_.begin(), sps_.end());
        au.insert(au.end(), pps_.begin(), pps_.end());
    }
    au.insert(au.end(), nalus.begin(), nalus.end());

    int64_t dts90k = static_cast<int64_t>(pkt.timestamp) * 90;
    int64_t pts90k = dts90k + static_cast<int64_t>(cts) * 90;
    hls_->onVideoFrame(au.data(), au.size(), pts90k, dts90k, pkt.isKeyframe);
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

    // FLV AAC: byte0 = 音频头(SoundFormat 等)，byte1 = AACPacketType(1)，byte2 起为 raw
    // （原实现从 byte1 开始，把 AACPacketType 当作 AAC 数据，导致解码大量报错）
    std::vector<uint8_t> adts =
        AacParser::wrapWithAdts(aacCfg_, d + 2, len - 2);
    hls_->onAudioFrame(adts.data(), adts.size(),
                       static_cast<int64_t>(pkt.timestamp) * 90);
}

}  // namespace streaming
