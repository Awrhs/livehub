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
