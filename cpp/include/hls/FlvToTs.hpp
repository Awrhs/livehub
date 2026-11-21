#pragma once

#include "hls/TsMuxer.hpp"
#include "rtmp/AacParser.hpp"
#include "rtmp/MediaStream.hpp"

#include <cstdint>
#include <vector>

namespace streaming {

/**
 * FlvToTs: 把 RTMP/FLV 形式的 MediaPacket（legacy AVC/AAC 与 Enhanced RTMP HEVC）
 * 转换为 MPEG-TS 写入 TsMuxer。用于 SRT 拉流等需要连续 TS 输出的场景。
 *
 * 处理要点（与 HLS 桥接一致）：
 *  - AVCC/HVCC 长度前缀 → Annex-B
 *  - TS 无带外 extradata：每个关键帧前内联 SPS/PPS（HEVC 为 VPS/SPS/PPS），每个 AU 前加 AUD
 *  - AAC raw → ADTS（FLV 音频负载 byte0 为音频头、byte1 为 AACPacketType）
 *  - 时间戳：毫秒 → 90kHz，PTS = DTS + CTS
 *  - 在拿到首个视频关键帧前丢弃视频（无参数集无法解码）
 */
class FlvToTs {
public:
    explicit FlvToTs(TsMuxer& muxer) : muxer_(muxer) {}

    void onPacket(const MediaPacket& pkt);

private:
    void onVideo(const MediaPacket& pkt);
    void onAudio(const MediaPacket& pkt);

    TsMuxer& muxer_;
    bool hevc_ = false;
    bool haveConfig_ = false;
    bool started_ = false;  // 已输出首个关键帧
    std::vector<uint8_t> vps_, sps_, pps_;
    AacParser::Config aac_{};
    bool haveAac_ = false;
};

}  // namespace streaming
