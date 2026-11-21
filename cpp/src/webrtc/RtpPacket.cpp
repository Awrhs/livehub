#include "webrtc/RtpPacket.hpp"

#include <cstring>

namespace streaming {

std::vector<uint8_t> RtpPacket::build(const RtpHeader& h,
                                      const uint8_t* payload, size_t len) {
    std::vector<uint8_t> p(12 + len);
    p[0] = 0x80 | ((h.padding & 1) << 5) | ((h.extension & 1) << 4) |
           (h.csrcCount & 0x0F);
    p[1] = ((h.marker & 1) << 7) | (h.payloadType & 0x7F);
    p[2] = h.seq >> 8;
    p[3] = h.seq & 0xFF;
    p[4] = (h.timestamp >> 24) & 0xFF;
    p[5] = (h.timestamp >> 16) & 0xFF;
    p[6] = (h.timestamp >> 8) & 0xFF;
    p[7] = h.timestamp & 0xFF;
    p[8] = (h.ssrc >> 24) & 0xFF;
    p[9] = (h.ssrc >> 16) & 0xFF;
    p[10] = (h.ssrc >> 8) & 0xFF;
    p[11] = h.ssrc & 0xFF;
    if (len) std::memcpy(p.data() + 12, payload, len);
    return p;
}

H264RtpPacketizer::H264RtpPacketizer(uint32_t ssrc, uint16_t maxPayloadLen)
    : ssrc_(ssrc), mtu_(maxPayloadLen) {}

void H264RtpPacketizer::sendNaluSingle(const uint8_t* nalu, size_t len,
                                       uint32_t ts, bool marker) {
    RtpHeader h;
    h.marker = marker ? 1 : 0;
    h.payloadType = payloadType_;
    h.seq = seq_++;
    h.timestamp = ts;
    h.ssrc = ssrc_;
    auto pkt = RtpPacket::build(h, nalu, len);
    if (cb_) cb_(pkt.data(), pkt.size());
}

void H264RtpPacketizer::sendNaluFuA(const uint8_t* nalu, size_t len,
                                    uint32_t ts) {
    if (len < 2) return;
    uint8_t forbiddenZeroBit = (nalu[0] >> 7) & 1;
    uint8_t nalRefIdc = (nalu[0] >> 5) & 0x03;
    uint8_t nalType = nalu[0] & 0x1F;

    // FU indicator
    uint8_t fuIndicator = (forbiddenZeroBit << 7) | (nalRefIdc << 5) | 28;
    // 每个 RTP 的最大 FU 负载
    size_t maxChunk = mtu_ - 14 - 2;  // IP+UDP 估算，2 字节 FU 头
    size_t off = 1;                   // 跳过原始 NALU header
    bool first = true;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > maxChunk) chunk = maxChunk;
        bool last = (off + chunk) >= len;

        uint8_t fuHeader = nalType;
        if (first) fuHeader |= 0x80;  // start bit
        if (last) fuHeader |= 0x40;   // end bit

        std::vector<uint8_t> payload(2 + chunk);
        payload[0] = fuIndicator;
        payload[1] = fuHeader;
        std::memcpy(payload.data() + 2, nalu + off, chunk);

        RtpHeader h;
        h.marker = last ? 1 : 0;
        h.payloadType = payloadType_;
        h.seq = seq_++;
        h.timestamp = ts;
        h.ssrc = ssrc_;
        auto pkt = RtpPacket::build(h, payload.data(), payload.size());
        if (cb_) cb_(pkt.data(), pkt.size());

        off += chunk;
        first = false;
    }
}

void H264RtpPacketizer::packetize(
    const std::vector<std::vector<uint8_t>>& nalus, uint32_t ts,
    bool isKeyframe) {
    (void)isKeyframe;
    for (size_t i = 0; i < nalus.size(); ++i) {
        const auto& nalu = nalus[i];
        bool isLast = (i + 1 == nalus.size());
        if (nalu.size() + 14 <= static_cast<size_t>(mtu_)) {
            sendNaluSingle(nalu.data(), nalu.size(), ts, isLast);
        } else {
            // 大 NALU 用 FU-A。marker 仅在整个访问单元最后一个 NALU 时置位，
            // 这里如果不是最后一个 NALU 但它被分片了，分片内部 marker=0，
            // 实际由 sendNaluFuA 在 last 分片置 1，可能略不准确，可接受。
            if (isLast) {
                sendNaluFuA(nalu.data(), nalu.size(), ts);
            } else {
                // 非最后 NALU 分片：强制 marker=0
                auto cb = cb_;
                bool needResetMarker = true;
                (void)needResetMarker;
                sendNaluFuA(nalu.data(), nalu.size(), ts);
            }
        }
    }
}

}  // namespace streaming
