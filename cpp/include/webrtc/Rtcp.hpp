#pragma once

#include <cstdint>
#include <vector>

namespace streaming {

enum : uint8_t {
    kRtcpSR = 200,
    kRtcpRR = 201,
    kRtcpSDES = 202,
    kRtcpPF = 205,  // transport-layer feedback (NACK)
    kRtcpPS = 206,  // payload-specific feedback (PLI/FIR)
};

/**
 * Rtcp: RTCP 报文构造（RFC 3550 / 4585）。
 *
 * 服务器作为发送方周期性发送 SR（含 NTP/RTP 时间戳与发送计数），
 * 解析客户端的 RR/NACK/PLI 做重传与关键帧请求处理（简化）。
 */
class Rtcp {
public:
    /// Sender Report
    static std::vector<uint8_t> buildSR(uint32_t ssrc, uint64_t ntpTs,
                                        uint32_t rtpTs, uint32_t packetCount,
                                        uint32_t octetCount);

    /// 解析 RR/NACK/PLI（返回 true 表示是 PLI，需要请求关键帧）
    struct Feedback {
        bool isPli = false;
        bool isNack = false;
        uint32_t senderSsrc = 0;
        uint32_t mediaSsrc = 0;
        std::vector<uint16_t> nackPids;
        // RR / SR 的接收报告块：被报告的 SSRC 与丢包率（fraction lost，0-255）
        struct ReportBlock {
            uint32_t ssrc = 0;
            uint8_t fractionLost = 0;
        };
        std::vector<ReportBlock> reports;
        // REMB（draft-alvestrand-rmcat-remb）：接收端带宽估计
        bool isRemb = false;
        uint64_t rembBps = 0;
        std::vector<uint32_t> rembSsrcs;
    };
    static bool parse(const uint8_t* data, size_t len, Feedback& fb);
};

}  // namespace streaming
