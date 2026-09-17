#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace streaming {

/**
 * H264RtpDepacketizer: 把 H264 RTP 流重组为 NALU（RFC 6184）。
 *
 * 处理 Single NAL / FU-A / STAP-A，处理乱序前的简单按序假设；
 * 每完成一个 NALU 通过回调返回（NALU 含头部字节，不含 start code）。
 */
class H264RtpDepacketizer {
public:
    using NaluCallback =
        std::function<void(const uint8_t* nalu, size_t len, uint32_t rtpTs,
                           bool marker)>;

    void setCallback(NaluCallback cb) { cb_ = std::move(cb); }

    /// 喂入一个 RTP 报文（已去除 SRTP 层）
    void feed(const uint8_t* data, size_t len);

private:
    void emitSingle(const uint8_t* nalu, size_t len, uint32_t ts, bool marker);
    void handleFuA(const uint8_t* data, size_t len, uint8_t fuIndicator,
                   uint32_t ts, bool marker);
    void handleStapA(const uint8_t* data, size_t len, uint32_t ts,
                     bool marker);

    NaluCallback cb_;
    // FU-A 重组缓冲
    uint16_t fuStartSeq_ = 0;
    uint8_t fuNalType_ = 0;
    uint8_t fuNalRefIdc_ = 0;
    uint32_t fuTs_ = 0;
    std::vector<uint8_t> fuBuffer_;
    bool fuInProgress_ = false;
};

}  // namespace streaming
