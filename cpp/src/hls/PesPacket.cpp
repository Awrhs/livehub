#include "hls/PesPacket.hpp"

#include <cstring>

namespace streaming {

// 写入 5 字节 PTS/DTS 字段（TS 标头扩展）
void PesPacket::writeTsField(std::vector<uint8_t>& out, uint8_t prefix,
                              int64_t ts) {
    // [4 bits prefix][4 bits ts32..30][1 marker][15 bits ts29..15][1 marker][15 bits ts14..0][1 marker]
    out.push_back(prefix << 4 |
                  static_cast<uint8_t>((ts >> 29) & 0x0E) | 0x01);
    out.push_back(static_cast<uint8_t>((ts >> 22) & 0xFF));
    out.push_back(static_cast<uint8_t>(((ts >> 14) & 0xFE) | 0x01));
    out.push_back(static_cast<uint8_t>((ts >> 7) & 0xFF));
    out.push_back(static_cast<uint8_t>(((ts << 1) & 0xFE) | 0x01));
}

std::vector<uint8_t> PesPacket::pack(uint8_t streamId, int64_t pts,
                                      int64_t dts,
                                      const uint8_t* payload, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(9 + 10 + len);  // start code 3 + header 6 + pts/dts 5/10

    // packet_start_code_prefix
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(streamId);

    bool hasDts = (dts >= 0 && dts != pts);

    // PES_packet_length 之后的字节数（含 header flags + PTS/DTS + payload）
    size_t headerDataLen = 3 + (hasDts ? 10 : 5);
    size_t afterLen = headerDataLen + len;
    // PES_packet_length 为 16-bit，视频可能超长，置 0 表示不限长
    uint16_t pesLen = afterLen <= 0xFFFF ? static_cast<uint16_t>(afterLen) : 0;
    out.push_back(static_cast<uint8_t>(pesLen >> 8));
    out.push_back(static_cast<uint8_t>(pesLen & 0xFF));

    // 0x80 = marker bits + original; 0x80 = PTS only, 0xC0 = PTS+DTS
    out.push_back(0x80);
    out.push_back(hasDts ? 0xC0 : 0x80);
    out.push_back(static_cast<uint8_t>(hasDts ? 10 : 5));  // header data length

    writeTsField(out, hasDts ? 0x03 : 0x02, pts);
    if (hasDts) {
        writeTsField(out, 0x01, dts);
    }

    if (payload && len > 0) {
        out.insert(out.end(), payload, payload + len);
    }
    return out;
}

}  // namespace streaming
