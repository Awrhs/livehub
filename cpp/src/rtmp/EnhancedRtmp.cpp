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
    std::memset(&out, 0, sizeof(out));
    if (len < 1) return false;
    out.frameType = frameType(data[0]);
    out.enhanced = isExVideo(data[0]);
    if (!out.enhanced) {
        out.codec = VideoCodec::kH264;  // legacy：交给 AvcParser
        return false;
    }
    out.packetType = data[0] & 0x0F;
    if (out.packetType == 6 || out.packetType == 7) return false;  // multitrack/ModEx 暂不支持
    if (len < 5) return false;
    out.fourcc = readFourcc(data + 1);
    out.codec = fourccToVideoCodec(out.fourcc);
    size_t off = 5;
    out.cts = 0;
    if (out.packetType == kPktCodedFrames &&
        (out.fourcc == kFourccHvc1 || out.fourcc == kFourccAvc1)) {
        if (len < off + 3) return false;
        int32_t cts = (int32_t(data[off]) << 16) |
                      (int32_t(data[off + 1]) << 8) | int32_t(data[off + 2]);
        if (cts & 0x800000) cts |= ~0x00FFFFFF;  // 符号扩展
        out.cts = cts;
        off += 3;
    }
    out.payload = data + off;
    out.payloadLen = len - off;
    return true;
}

bool EnhancedRtmp::parseAudio(const uint8_t* data, size_t len, AudioInfo& out) {
    std::memset(&out, 0, sizeof(out));
    if (len < 1) return false;
    out.enhanced = isExAudio(data[0]);
    if (!out.enhanced) return false;
    out.packetType = data[0] & 0x0F;
    if (out.packetType == 5 || out.packetType == 7) return false;  // multitrack/ModEx
    if (len < 5) return false;
    out.fourcc = readFourcc(data + 1);
    out.codec = fourccToAudioCodec(out.fourcc);
    out.payload = data + 5;
    out.payloadLen = len - 5;
    return true;
}

std::vector<uint8_t> EnhancedRtmp::buildVideoMessage(uint32_t fourcc,
                                                     uint8_t frameType,
                                                     uint8_t packetType,
                                                     int32_t cts,
                                                     const uint8_t* payload,
                                                     size_t len) {
    bool hasCts = packetType == kPktCodedFrames &&
                  (fourcc == kFourccHvc1 || fourcc == kFourccAvc1);
    size_t hdr = hasCts ? 8 : 5;
    std::vector<uint8_t> out(hdr + len);
    out[0] = static_cast<uint8_t>(0x80 | ((frameType & 0x07) << 4) | (packetType & 0x0F));
    writeFourcc(fourcc, &out[1]);
    if (hasCts) {
        uint32_t u = static_cast<uint32_t>(cts);
        out[5] = (u >> 16) & 0xFF;
        out[6] = (u >> 8) & 0xFF;
        out[7] = u & 0xFF;
    }
    if (len) std::memcpy(out.data() + hdr, payload, len);
    return out;
}

std::vector<uint8_t> EnhancedRtmp::buildAudioMessage(uint32_t fourcc,
                                                     uint8_t packetType,
                                                     const uint8_t* payload,
                                                     size_t len) {
    std::vector<uint8_t> out(5 + len);
    out[0] = static_cast<uint8_t>(0x90 | (packetType & 0x0F));
    writeFourcc(fourcc, &out[1]);
    if (len) std::memcpy(out.data() + 5, payload, len);
    return out;
}

}  // namespace streaming
