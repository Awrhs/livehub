#pragma once

#include "hls/TsPacket.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace streaming {

/**
 * TsMuxer: 把编码后的 AAC/AVC 帧切成 188 字节 TS 包。
 *
 * 职责：
 * - 周期性（每 pat/patInterval 个 TS 包）发送 PAT、PMT
 * - 维护各 PID 的 continuity counter
 * - 把 PES 数据按 188 字节分片（首包带 PUSI + PCR）
 * - 每个输出包通过 outputCb 交给 Segmenter 写文件
 *
 * 简历亮点：TS 复用实现、PID/CC 管理、PCR 时钟
 */
class TsMuxer {
public:
    using OutputCallback = std::function<void(const uint8_t*, size_t)>;

    explicit TsMuxer(OutputCallback outputCb);

    /// 设置各 PID（默认 0x1000/0x0100/0x0101）
    void setPmtPid(uint16_t pid) { pmtPid_ = pid; }
    void setVideoPid(uint16_t pid) { videoPid_ = pid; }
    void setAudioPid(uint16_t pid) { audioPid_ = pid; }
    void setPcrPid(uint16_t pid) { pcrPid_ = pid; }

    /// 设置视频流类型（kStreamTypeVideo=AVC，kStreamTypeHevc=HEVC）。默认 AVC。
    void setVideoStreamType(uint8_t t) { videoStreamType_ = t; }

    /// 写入一帧视频（AVC/HEVC Annex-B）。pts/dts 为 90kHz。isKeyframe 触发 PAT/PMT 重发。
    void writeVideo(const uint8_t* data, size_t len, int64_t pts, int64_t dts,
                    bool isKeyframe);

    /// 写入一帧音频（AAC ADTS）。pts 为 90kHz。
    void writeAudio(const uint8_t* data, size_t len, int64_t pts);

    uint64_t packetsWritten() const { return packetsWritten_; }

private:
    void writePatPmt(int64_t pcr);
    void writePesPackets(uint16_t pid, const std::vector<uint8_t>& pes,
                         int64_t pcr, bool randomAccess);

    OutputCallback outputCb_;
    uint16_t pmtPid_;
    uint16_t pcrPid_;
    uint16_t videoPid_;
    uint16_t audioPid_;
    uint8_t videoStreamType_ = kStreamTypeVideo;

    uint8_t patCc_ = 0;
    uint8_t pmtCc_ = 0;
    uint8_t videoCc_ = 0;
    uint8_t audioCc_ = 0;

    uint64_t packetsWritten_ = 0;
    uint64_t sincePatPmt_ = 9999;  // 首次立即发送 PAT/PMT
    static const uint64_t kPatPmtInterval = 40;  // 约每 40 个 TS 包
};

}  // namespace streaming
