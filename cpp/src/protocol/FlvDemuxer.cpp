#include "protocol/FlvDemuxer.hpp"
#include "rtmp/EnhancedRtmp.hpp"

#include <cstring>

namespace streaming {

void FlvDemuxer::reset() {
    buf_.retrieveAll();
    headerParsed_ = false;
}

void FlvDemuxer::feed(const uint8_t* data, size_t len) {
    // 输入为纯 FLV 字节流（HTTP 头由调用方处理）。
    // 原实现在此再次“跳过 HTTP 头”：在已剥离头部的二进制 FLV 中搜索 \r\n\r\n，
    // 丢弃之前的全部数据，回源拉流完全无法解析。
    buf_.append(reinterpret_cast<const char*>(data), len);

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
            uint8_t b0 = pkt.data[0];
            pkt.isKeyframe = EnhancedRtmp::frameType(b0) == 1;
            if (EnhancedRtmp::isExVideo(b0)) {  // Enhanced RTMP（HEVC 等）
                EnhancedRtmp::VideoInfo vi;
                if (EnhancedRtmp::parseVideo(pkt.data.data(), dataSize, vi)) {
                    pkt.enhanced = true;
                    pkt.videoCodec = vi.codec;
                    pkt.enhancedPacketType = vi.packetType;
                    pkt.isSequenceHeader = vi.packetType == EnhancedRtmp::kPktSequenceStart;
                }
            } else if ((b0 & 0x0F) == 7) {
                pkt.videoCodec = VideoCodec::kH264;
                pkt.isSequenceHeader = dataSize >= 2 && pkt.data[1] == 0;
            }
        } else if (pkt.type == MediaPacket::kAudio && dataSize >= 2) {
            if ((pkt.data[0] >> 4) == 10) {
                pkt.audioCodec = AudioCodec::kAac;
                pkt.isSequenceHeader = pkt.data[1] == 0;
            }
            pkt.isKeyframe = true;
        }
        if (onTag_) onTag_(pkt);
    }
    buf_.retrieve(total);
    return true;
}

}  // namespace streaming
