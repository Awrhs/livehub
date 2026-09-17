#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace streaming {

/**
 * RtpPacket: RTP 报文构造（RFC 3550）。
 *
 * 用于把 NALU 打包成 RTP 发送（H264），并解析客户端 RTCP。
 * 序列号、时间戳、SSRC 由发送方维护。
 */
struct RtpHeader {
    uint8_t version = 2;
    uint8_t padding = 0;
    uint8_t extension = 0;
    uint8_t csrcCount = 0;
    uint8_t marker = 0;
    uint8_t payloadType = 0;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
};

class RtpPacket {
public:
    /// 构造单个 RTP 报文
    static std::vector<uint8_t> build(const RtpHeader& hdr,
                                      const uint8_t* payload, size_t len);
};

/**
 * H264RtpPacketizer: 把 Annex-B/AVCC NALU 打成 RTP。
 *
 * - 小 NALU 用 Single NAL Unit packet；
 * - 大 NALU 用 FU-A 分片（RFC 6184）。
 *
 * 输入为「一个 NALU（不含 start code）」，输出通过回调发送每个 RTP 报文。
 * 视频时钟 90kHz。
 */
class H264RtpPacketizer {
public:
    using SendCallback =
        std::function<void(const uint8_t* data, size_t len)>;

    H264RtpPacketizer(uint32_t ssrc, uint16_t maxPayloadLen = 1200);

    void setPayloadType(uint8_t pt) { payloadType_ = pt; }
    void setCallback(SendCallback cb) { cb_ = std::move(cb); }

    /// 发送一帧访问单元的所有 NALU（nalus 不含 start code）。
    /// 第一个 NALU 通常是 SPS/PPS 等，最后一个为 IDR/non-IDR slice。
    /// isKeyframe 控制 marker 前的关键帧标记，timestamp90k 为 RTP 时间戳。
    void packetize(const std::vector<std::vector<uint8_t>>& nalus,
                   uint32_t timestamp90k, bool isKeyframe);

    uint16_t nextSeq() { return seq_++; }

private:
    void sendNaluSingle(const uint8_t* nalu, size_t len, uint32_t ts,
                        bool marker);
    void sendNaluFuA(const uint8_t* nalu, size_t len, uint32_t ts);

    uint32_t ssrc_;
    uint16_t seq_ = 0;
    uint8_t payloadType_ = 96;
    uint16_t mtu_;
    SendCallback cb_;
};

}  // namespace streaming
