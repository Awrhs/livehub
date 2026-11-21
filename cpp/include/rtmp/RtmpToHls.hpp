#pragma once

#include "core/NonCopyable.hpp"
#include "hls/HlsManager.hpp"
#include "rtmp/AacParser.hpp"
#include "rtmp/AvcParser.hpp"
#include "rtmp/MediaStream.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace streaming {

/**
 * RtmpToHls: 把一路 RTMP MediaStream 桥接到 HLS。
 *
 * 订阅 MediaStream 的实时包：
 * - type9 视频：解析 FrameType/AVCPacketType/CTS，AVCC→Annex-B，喂 HlsSource；
 *   sequence header 提取 SPS/PPS（用于 muxer 配置）。
 * - type8 音频：AAC sequence header 解析 ASC，raw 帧加 ADTS 后喂 HlsSource。
 *
 * RTMP 时间戳单位为毫秒，HLS 用 90kHz，转换乘 90。
 */
class RtmpToHls : NonCopyable {
public:
    RtmpToHls(std::shared_ptr<MediaStream> stream, HlsManager* manager,
              std::string outputDir, int targetDuration = 6,
              int windowSize = 5);
    ~RtmpToHls();

    void start();
    void stop();

    const std::string& streamKey() const { return stream_->key(); }
    std::shared_ptr<HlsSource> hlsSource() const { return hls_; }

private:
    void onPacket(const MediaPacket& pkt);
    void onVideo(const MediaPacket& pkt);
    void onAudio(const MediaPacket& pkt);

    std::shared_ptr<MediaStream> stream_;
    HlsManager* manager_;
    std::string outputDir_;
    int targetDuration_;
    int windowSize_;

    std::shared_ptr<HlsSource> hls_;
    MediaStream::SubscriberId subId_ = 0;

    bool hasVideoConfig_ = false;
    std::vector<uint8_t> vps_;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    VideoCodec videoCodec_ = VideoCodec::kH264;

    AacParser::Config aacCfg_;
    bool hasAudioConfig_ = false;
};

}  // namespace streaming
