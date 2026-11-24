#include "webrtc/Rtcp.hpp"

#include <cstring>

namespace streaming {

static void putU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24);
    v.push_back(x >> 16);
    v.push_back(x >> 8);
    v.push_back(x & 0xFF);
}

std::vector<uint8_t> Rtcp::buildSR(uint32_t ssrc, uint64_t ntpTs,
                                   uint32_t rtpTs, uint32_t packetCount,
                                   uint32_t octetCount) {
    std::vector<uint8_t> p;
    p.reserve(28);
    // V=2, P=0, RC=0, PT=200(SR), length=6 (28 字节总长)
    p.push_back(0x80);
    p.push_back(kRtcpSR);
    p.push_back(0);
    p.push_back(6);
    putU32(p, ssrc);
    // NTP timestamp: 64-bit (upper 32 seconds, lower 32 fraction)
    putU32(p, static_cast<uint32_t>(ntpTs >> 32));
    putU32(p, static_cast<uint32_t>(ntpTs & 0xFFFFFFFF));
    putU32(p, rtpTs);
    putU32(p, packetCount);
    putU32(p, octetCount);
    return p;
}

bool Rtcp::parse(const uint8_t* data, size_t len, Feedback& fb) {
    if (len < 8) return false;
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) return false;
    uint8_t pt = data[1];
    // uint16_t lengthWords = (uint16_t(data[2]) << 8) | data[3];
    auto rd32 = [data](size_t off) {
        return (uint32_t(data[off]) << 24) | (uint32_t(data[off + 1]) << 16) |
               (uint32_t(data[off + 2]) << 8) | uint32_t(data[off + 3]);
    };
    if (pt == kRtcpRR || pt == kRtcpSR) {
        // RR：头 8 字节后为报告块；SR：另有 20 字节发送者信息。每块 24 字节
        size_t off = pt == kRtcpSR ? 28 : 8;
        uint8_t rc = data[0] & 0x1F;
        fb.senderSsrc = rd32(4);
        for (uint8_t i = 0; i < rc && off + 24 <= len; ++i, off += 24) {
            fb.reports.push_back({rd32(off), data[off + 4]});
        }
        return !fb.reports.empty();
    }
    if (len < 12) return false;
    fb.senderSsrc = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) |
                    (uint32_t(data[6]) << 8) | uint32_t(data[7]);
    fb.mediaSsrc = (uint32_t(data[8]) << 24) | (uint32_t(data[9]) << 16) |
                   (uint32_t(data[10]) << 8) | uint32_t(data[11]);

    if (pt == kRtcpPS && len >= 13) {
        uint8_t fmt = data[0] & 0x1F;  // FMT
        if (fmt == 1) {
            // PLI
            fb.isPli = true;
            return true;
        }
        if (fmt == 4) {
            // FIR（关键帧请求），按 PLI 处理
            fb.isPli = true;
            return true;
        }
        if (fmt == 15 && len >= 20 && data[12] == 'R' && data[13] == 'E' && data[14] == 'M' &&
            data[15] == 'B') {
            // REMB：Num SSRC(8) | BR Exp(6) | BR Mantissa(18) | SSRC...
            uint8_t num = data[16];
            uint8_t exp = data[17] >> 2;
            uint32_t mant = (uint32_t(data[17] & 0x03) << 16) | (uint32_t(data[18]) << 8) | data[19];
            fb.isRemb = true;
            fb.rembBps = exp >= 46 ? UINT64_MAX : (uint64_t(mant) << exp);
            for (uint8_t i = 0; i < num && 20 + 4 * (size_t(i) + 1) <= len; ++i)
                fb.rembSsrcs.push_back(rd32(20 + 4 * size_t(i)));
            return true;
        }
    } else if (pt == kRtcpPF) {
        uint8_t fmt = data[0] & 0x1F;  // FMT=1 NACK
        if (fmt == 1) {
            fb.isNack = true;
            // 每个 FCI 4 字节: PID(16) + BLP(16)
            size_t fciLen = len - 12;
            for (size_t off = 0; off + 4 <= fciLen; off += 4) {
                uint16_t pid = (uint16_t(data[12 + off]) << 8) |
                               data[12 + off + 1];
                uint16_t blp = (uint16_t(data[12 + off + 2]) << 8) |
                               data[12 + off + 3];
                fb.nackPids.push_back(pid);
                for (int i = 0; i < 16; ++i) {
                    if (blp & (1 << i)) fb.nackPids.push_back(pid + 1 + i);
                }
            }
            return true;
        }
    }
    return false;
}

}  // namespace streaming
