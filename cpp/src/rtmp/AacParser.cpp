#include "rtmp/AacParser.hpp"

#include <cstring>

namespace streaming {

AacParser::Config AacParser::parseAudioSpecificConfig(const uint8_t* d,
                                                      size_t len) {
    Config cfg;
    if (len < 2) return cfg;
    // audioObjectType (5 bits)
    uint32_t bits = (static_cast<uint32_t>(d[0]) << 8) |
                    static_cast<uint32_t>(d[1]);
    cfg.profile = (bits >> 11) & 0x1F;
    cfg.sampleRateIndex = (bits >> 7) & 0x0F;
    cfg.channelConfig = (bits >> 3) & 0x0F;
    cfg.valid = (cfg.profile >= 1 && cfg.profile <= 4) &&
                cfg.sampleRateIndex < 13 && cfg.channelConfig > 0;
    return cfg;
}

bool AacParser::isSequenceHeader(const uint8_t* data, size_t len) {
    return len >= 2 && (data[0] == 0xAF || data[0] == 0xAE) && data[1] == 0;
}

int AacParser::sampleRateFromIndex(int idx) {
    static const int table[] = {96000, 88200, 64000, 48000, 44100,
                                 32000, 24000, 22050, 16000, 12000,
                                 11025, 8000,  7350};
    if (idx < 0 || idx >= 13) return 0;
    return table[idx];
}

std::vector<uint8_t> AacParser::makeAdtsHeader(const Config& cfg,
                                                 size_t frameLen) {
    // frameLen 含 ADTS 头（7 字节）
    size_t fullLen = frameLen + 7;
    std::vector<uint8_t> hdr(7, 0);
    hdr[0] = 0xFF;
    hdr[1] = 0xF1;  // MPEG-4, layer 0, no CRC
    hdr[2] = static_cast<uint8_t>(
        ((cfg.profile - 1) << 6) |            // profile (AAC LC=1 -> 0)
        (cfg.sampleRateIndex << 2) |
        ((cfg.channelConfig >> 2) & 0x01));
    hdr[3] = static_cast<uint8_t>(
        ((cfg.channelConfig & 0x03) << 6) |
        ((fullLen >> 11) & 0x03));
    hdr[4] = static_cast<uint8_t>((fullLen >> 3) & 0xFF);
    hdr[5] = static_cast<uint8_t>(((fullLen & 0x07) << 5) | 0x1F);
    hdr[6] = 0xFC;  // buffer fullness = 0x7FF, number_of_raw_frames = 0
    return hdr;
}

std::vector<uint8_t> AacParser::wrapWithAdts(const Config& cfg,
                                               const uint8_t* frame,
                                               size_t len) {
    std::vector<uint8_t> out = makeAdtsHeader(cfg, len);
    out.insert(out.end(), frame, frame + len);
    return out;
}

}  // namespace streaming
