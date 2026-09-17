#include "rtmp/EnhancedRtmp.hpp"

#include <cstring>

namespace streaming {

namespace {
VideoCodec fourccToVideoCodec(uint32_t f) {
    switch (f) {
        case EnhancedRtmp::kFourccHvc1: return VideoCodec::kHevc;
        case EnhancedRtmp::kFourccAvc1: return VideoCodec::kH264;
        // av01/vp09 暂不做后续解复用，但识别
        default: return VideoCodec::kUnknown;
    }
}
AudioCodec fourccToAudioCodec(uint32_t f) {
    switch (f) {
        case EnhancedRtmp::kFourccOpus: return AudioCodec::kOpus;
        case EnhancedRtmp::kFourccMp4a: return AudioCodec::kAac;
        default: return AudioCodec::kUnknown;
    }
}
}  // namespace

bool EnhancedRtmp::parseVideo(const uint8_t* data, size_t len, VideoInfo& out) {
    if (len < 5) return false;
    std::memset(&out, 0, sizeof(out));
    out.frameType = frameType(data[0]);
    out.enhanced = isExHeader(data[0]);
    if (!out.enhanced) {
        // legacy AVC：交给 AvcParser 处理；这里仅标记
        out.codec = VideoCodec::kH264;
        return false;
    }
    out.fourcc = readFourcc(data + 1);
    out.codec = fourccToVideoCodec(out.fourcc);
    out.packetType = data[5];
    size_t off = 6;
    // CodedFrames(1) 有 3 字节 CTS；CodedFramesX(2) 无
    if (out.packetType == 1) {
        if (len < off + 3) return false;
        int32_t cts = (int32_t(data[off]) << 16) |
                      (int32_t(data[off + 1]) << 8) | int32_t(data[off + 2]);
        if (cts & 0x800000) cts |= ~0x00FFFFFF;  // 符号扩展
        out.cts = cts;
        off += 3;
    } else {
        out.cts = 0;
    }
    out.payload = data + off;
    out.payloadLen = len - off;
    return true;
}

bool EnhancedRtmp::parseAudio(const uint8_t* data, size_t len, AudioInfo& out) {
    if (len < 5) return false;
    std::memset(&out, 0, sizeof(out));
    out.enhanced = isExHeader(data[0]);
    if (!out.enhanced) return false;
    out.fourcc = readFourcc(data + 1);
    out.codec = fourccToAudioCodec(out.fourcc);
    out.packetType = data[5];
    out.payload = data + 6;
    out.payloadLen = len - 6;
    return true;
}

std::vector<uint8_t> EnhancedRtmp::buildVideoMessage(uint32_t fourcc,
                                                     uint8_t frameType,
                                                     uint8_t packetType,
                                                     int32_t cts,
                                                     const uint8_t* payload,
                                                     size_t len) {
    bool hasCts = (packetType == 1);
    size_t hdr = hasCts ? 9 : 6;
    std::vector<uint8_t> out(hdr + len);
    out[0] = static_cast<uint8_t>((frameType << 4) | 0x09);
    writeFourcc(fourcc, &out[1]);
    out[5] = packetType;
    if (hasCts) {
        uint32_t u = static_cast<uint32_t>(cts);
        out[6] = (u >> 16) & 0xFF;
        out[7] = (u >> 8) & 0xFF;
        out[8] = u & 0xFF;
    }
    if (len) std::memcpy(out.data() + hdr, payload, len);
    return out;
}

std::vector<uint8_t> EnhancedRtmp::buildAudioMessage(uint32_t fourcc,
                                                     uint8_t packetType,
                                                     const uint8_t* payload,
                                                     size_t len) {
    std::vector<uint8_t> out(6 + len);
    out[0] = 0x09;
    writeFourcc(fourcc, &out[1]);
    out[5] = packetType;
    if (len) std::memcpy(out.data() + 6, payload, len);
    return out;
}

}  // namespace streaming
