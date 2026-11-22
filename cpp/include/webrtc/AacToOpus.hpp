#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace streaming {

/**
 * AacToOpus: 把 RTMP/FLV 中的 AAC 音频转码为 Opus（WebRTC 唯一普遍支持的音频编码）。
 *
 * 流程：AAC 解码（libavcodec）→ 重采样为 48kHz 双声道（libswresample）→
 *       按 20ms（960 样本）分帧 → Opus 编码（libopus，缺失时用 FFmpeg 内置 opus 编码器）。
 * 输出回调携带 Opus 包与 48kHz 时间戳（RTP 时钟）。
 *
 * 未链接 libav（HAVE_LIBAV 未定义）时 init() 返回 false，WebRTC 仅发送视频。
 * 非线程安全：同一实例只能在一个线程中使用（发布者线程）。
 */
class AacToOpus {
public:
    using OutputCallback = std::function<void(const uint8_t* data, size_t len, uint32_t ts48k)>;

    AacToOpus();
    ~AacToOpus();
    AacToOpus(const AacToOpus&) = delete;
    AacToOpus& operator=(const AacToOpus&) = delete;

    /// 用 AudioSpecificConfig（AAC 序列头负载）初始化；可重复调用（配置变化时重建）
    bool init(const uint8_t* asc, size_t len);
    bool ready() const;
    void setCallback(OutputCallback cb) { cb_ = std::move(cb); }

    /// 输入一帧原始 AAC（不含 ADTS 头），timestampMs 为 RTMP 时间戳
    void feed(const uint8_t* data, size_t len, uint32_t timestampMs);

    /// 是否具备转码能力（编译时链接了 libav 且找到 Opus 编码器）
    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    OutputCallback cb_;
};

}  // namespace streaming
