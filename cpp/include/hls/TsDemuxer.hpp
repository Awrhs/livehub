#pragma once

#include "core/NonCopyable.hpp"
#include "rtmp/MediaStream.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace streaming {

/**
 * TsDemuxer: 把 MPEG-TS (188B) 解复用为 MediaPacket，喂给 MediaStream。
 *
 * 用于 SRT ingest / 回源：网络侧是 TS，业务侧用 MediaStream 分发。
 *
 * 处理：
 *  - PAT(0x0000) 定位 PMT PID
 *  - PMT 解析 video/audio PID 及 stream_type（H.264 0x1B / H.265 0x24 / AAC 0x0F）
 *  - PES 重组，解析 PTS/DTS
 *  - 视频 Annex-B -> AVCC/hvCC 风格 RTMP 包（构造 sequence header + 长度前缀 NALU）
 *  - 音频 ADTS -> ASC sequence header + raw AAC
 *
 * 简化实现：聚焦单视频流 + 单音频流，足够直播 ingest。
 */
class TsDemuxer : NonCopyable {
public:
    using PacketCallback = std::function<void(const MediaPacket&)>;

    void setPacketCallback(PacketCallback cb) { cb_ = std::move(cb); }

    /// 喂入任意长度字节（内部按 188 切包，支持跨调用残留）
    void feed(const uint8_t* data, size_t len);

private:
    struct PidState {
        bool seen = false;
        uint8_t streamType = 0;
        bool isVideo = false;
        std::vector<uint8_t> pes;  // PES 负载累积
        bool pesStart = false;
        int64_t pts = -1;
        int64_t dts = -1;
        // 视频配置提取
        std::vector<uint8_t> vps, sps, pps;
        bool configSent = false;
        // 音频
        std::vector<uint8_t> asc;
        bool ascSent = false;
        int sampleRate = 44100;
        int channels = 2;
    };

    void handlePacket(const uint8_t* p);
    void handlePes(PidState& st);
    void emitVideo(PidState& st);
    void emitAudio(PidState& st);
    void flushVideoConfig(PidState& st, bool hevc);
    void flushAudioConfig(PidState& st);

    PacketCallback cb_;
    uint16_t pmtPid_ = 0x1000;
    bool pmtKnown_ = false;
    // PID -> state
    std::vector<PidState> pids_{8192};
    std::vector<uint8_t> leftover_;

    uint32_t videoFirstPts_ = 0;
    bool videoHaveFirst_ = false;
    uint32_t audioFirstPts_ = 0;
    bool audioHaveFirst_ = false;
};

}  // namespace streaming
