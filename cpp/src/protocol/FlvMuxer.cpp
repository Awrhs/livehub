#include "protocol/FlvMuxer.hpp"

#include "rtmp/Amf0.hpp"

#include <cstring>

namespace streaming {

namespace {
void putUInt24(Buffer& out, uint32_t v) {
    char b[3] = {static_cast<char>((v >> 16) & 0xFF),
                 static_cast<char>((v >> 8) & 0xFF),
                 static_cast<char>(v & 0xFF)};
    out.append(b, 3);
}
void putUInt32(Buffer& out, uint32_t v) {
    out.appendInt32(static_cast<int32_t>(v));
}
}  // namespace

void FlvMuxer::writeHeader(Buffer& out, bool hasAudio, bool hasVideo) {
    out.append("FLV", 3);
    out.appendInt8(1);  // version
    uint8_t flags = 0;
    if (hasAudio) flags |= 0x04;
    if (hasVideo) flags |= 0x01;
    out.appendInt8(static_cast<int8_t>(flags));
    putUInt32(out, kFlvHeaderSize);  // DataOffset = 9
    putUInt32(out, 0);               // PreviousTagSize0
}

void FlvMuxer::writeTag(Buffer& out, uint8_t tagType, uint32_t timestamp,
                        const uint8_t* data, size_t len) {
    // Tag header 11 字节
    out.appendInt8(static_cast<int8_t>(tagType & 0x1F));
    putUInt24(out, static_cast<uint32_t>(len));
    putUInt24(out, timestamp & 0xFFFFFF);        // 低 24 位时间戳
    out.appendInt8(static_cast<int8_t>(
        (timestamp >> 24) & 0xFF));              // 时间戳扩展字节
    putUInt24(out, 0);                           // stream id 恒为 0
    if (data && len > 0) {
        out.append(reinterpret_cast<const char*>(data), len);
    }
    putUInt32(out, static_cast<uint32_t>(11 + len));  // PreviousTagSize
}

void FlvMuxer::writeScriptTag(Buffer& out, const Amf0Value::Object& meta) {
    Buffer body;
    Amf0Value::str("onMetaData").encode(body);
    Amf0Value::ecmaArray(meta).encode(body);
    writeTag(out, 18, 0,
             reinterpret_cast<const uint8_t*>(body.peek()),
             body.readableBytes());
}

void FlvMuxer::writeMediaTag(Buffer& out, const MediaPacket& pkt) {
    uint8_t tagType = 0;
    switch (pkt.type) {
        case MediaPacket::kAudio: tagType = 8; break;
        case MediaPacket::kVideo: tagType = 9; break;
        case MediaPacket::kScript: tagType = 18; break;
    }
    writeTag(out, tagType, pkt.timestamp,
             pkt.data.empty() ? nullptr : pkt.data.data(),
             pkt.data.size());
}

}  // namespace streaming
