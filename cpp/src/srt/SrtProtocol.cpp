#include "srt/SrtProtocol.hpp"

#include <arpa/inet.h>

#include <chrono>
#include <cstring>

namespace streaming {

namespace {
const auto kBootTime = std::chrono::steady_clock::now();
}

uint32_t srtNowUs() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - kBootTime)
            .count());
}

uint32_t srtDataSeq(const uint8_t* p) {
    return ntohl(*reinterpret_cast<const uint32_t*>(p)) & 0x7FFFFFFFu;
}

uint16_t srtCtrlType(const uint8_t* p) {
    // 控制包 type 位于 field0 的 bit16..30（大端序下即 bytes 0..1 的低 15 位）；
    // bit31 为控制标志位。bytes 2..3 是 info/subtype，不是 type。
    uint16_t hi = ntohs(*reinterpret_cast<const uint16_t*>(p));
    return static_cast<uint16_t>(hi & 0x7FFFu);
}

std::vector<uint8_t> srtBuildControl(SrtCtrlType type, uint32_t info,
                                     uint32_t timestamp,
                                     uint32_t destSocket) {
    std::vector<uint8_t> out(kSrtHeaderLen);
    // SRT 控制包头：bit31=1，bits30..16=type，bits15..0 保留(0)
    uint32_t field0 = 0x80000000u | (static_cast<uint32_t>(type) << 16);
    uint32_t n;
    n = htonl(field0);
    std::memcpy(out.data(), &n, 4);
    n = htonl(timestamp);
    std::memcpy(out.data() + 4, &n, 4);
    n = htonl(destSocket);
    std::memcpy(out.data() + 8, &n, 4);
    // bytes 12-15: Additional Info Field（完整 32 位，大端）。
    // ACK 用它承载 ACK 序号，握手承载扩展字段长度等。
    n = htonl(info);
    std::memcpy(out.data() + 12, &n, 4);
    return out;
}

std::vector<uint8_t> srtBuildData(uint32_t seqno, uint32_t timestamp,
                                  uint32_t srcSocket, const uint8_t* payload,
                                  size_t len) {
    std::vector<uint8_t> out(kSrtHeaderLen + len);
    uint32_t n;
    n = htonl(seqno & 0x7FFFFFFFu);
    std::memcpy(out.data(), &n, 4);
    n = htonl(timestamp);
    std::memcpy(out.data() + 4, &n, 4);
    n = htonl(srcSocket);
    std::memcpy(out.data() + 8, &n, 4);
    if (len > 0) std::memcpy(out.data() + kSrtHeaderLen, payload, len);
    return out;
}

}  // namespace streaming
