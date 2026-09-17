#include "protocol/FlvDemuxer.hpp"

#include <cstring>

namespace streaming {

void FlvDemuxer::reset() {
    buf_.retrieveAll();
    headerParsed_ = false;
    headersSkipped_ = false;
}

void FlvDemuxer::feed(const uint8_t* data, size_t len) {
    buf_.append(reinterpret_cast<const char*>(data), len);

    // 若是 HTTP 流式响应，先跳过响应头
    if (!headersSkipped_) {
        const char* p = buf_.peek();
        size_t n = buf_.readableBytes();
        static const char kSep[] = "\r\n\r\n";
        for (size_t i = 0; i + 4 <= n; ++i) {
            if (std::memcmp(p + i, kSep, 4) == 0) {
                buf_.retrieve(i + 4);
                headersSkipped_ = true;
                break;
            }
        }
        if (!headersSkipped_) return;
    }

    while (buf_.readableBytes() > 0) {
        if (!headerParsed_) {
            if (!parseHeader()) break;
        }
        if (!parseTag()) break;
    }
}

bool FlvDemuxer::parseHeader() {
    if (buf_.readableBytes() < 13) return false;  // 9 + 4
    const char* p = buf_.peek();
    if (std::memcmp(p, "FLV", 3) != 0) {
        // 不是 FLV，丢弃 13 字节继续
        buf_.retrieve(13);
        return false;
    }
    buf_.retrieve(13);
    headerParsed_ = true;
    return true;
}

bool FlvDemuxer::parseTag() {
    if (buf_.readableBytes() < 11) return false;
    const uint8_t* h = reinterpret_cast<const uint8_t*>(buf_.peek());
    uint8_t tagType = h[0] & 0x1F;
    uint32_t dataSize = (static_cast<uint32_t>(h[1]) << 16) |
                        (static_cast<uint32_t>(h[2]) << 8) |
                        static_cast<uint32_t>(h[3]);
    uint32_t ts = (static_cast<uint32_t>(h[4]) << 16) |
                  (static_cast<uint32_t>(h[5]) << 8) |
                  static_cast<uint32_t>(h[6]) |
                  (static_cast<uint32_t>(h[7]) << 24);
    // h[8..10] stream id，忽略

    size_t total = 11 + dataSize + 4;
    if (buf_.readableBytes() < total) return false;

    if (tagType == 8 || tagType == 9 || tagType == 18) {
        MediaPacket pkt;
        pkt.type = (tagType == 8)   ? MediaPacket::kAudio
                   : (tagType == 9) ? MediaPacket::kVideo
                                    : MediaPacket::kScript;
        pkt.timestamp = ts;
        pkt.data.resize(dataSize);
        std::memcpy(pkt.data.data(), h + 11, dataSize);
        if (pkt.type == MediaPacket::kVideo && dataSize >= 1) {
            pkt.isKeyframe = (pkt.data[0] >> 4) == 0x01;
            pkt.isSequenceHeader = (pkt.data[0] & 0x0F) == 7 &&
                                   dataSize >= 2 && pkt.data[1] == 0;
        } else if (pkt.type == MediaPacket::kAudio && dataSize >= 2) {
            pkt.isSequenceHeader = (pkt.data[0] == 0xAF ||
                                    pkt.data[0] == 0xAE) &&
                                   pkt.data[1] == 0;
        }
        if (onTag_) onTag_(pkt);
    }
    buf_.retrieve(total);
    return true;
}

}  // namespace streaming
