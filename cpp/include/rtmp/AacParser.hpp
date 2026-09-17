#pragma once

#include <cstdint>
#include <vector>

namespace streaming {

/**
 * AacParser: AAC 音频解析。
 *
 * RTMP AAC 音频消息：
 *   byte0 = 0xAF（AAC, 44kHz, stereo, 16-bit）
 *   byte1 = AACPacketType: 0=AudioSpecificConfig(sequence header), 1=raw AAC
 *   byte2.. = AudioSpecificConfig（2 字节）或 raw AAC frame
 *
 * TS 中 AAC 需要 ADTS 头（7 字节），本类从 ASC 解析 profile/sampleRate/channels
 * 并为每个 raw frame 生成 ADTS 头。
 */
class AacParser {
public:
    struct Config {
        int profile = 1;        // AAC LC = 1
        int sampleRateIndex = 4; // 44100 = 4
        int channelConfig = 2;
        bool valid = false;
    };

    static Config parseAudioSpecificConfig(const uint8_t* data, size_t len);
    static bool isSequenceHeader(const uint8_t* data, size_t len);

    /// 为 raw AAC frame 生成 ADTS 头（7 字节）
    static std::vector<uint8_t> makeAdtsHeader(const Config& cfg,
                                                size_t frameLen);

    /// 给 raw AAC frame 加 ADTS 头，返回完整 ADTS frame
    static std::vector<uint8_t> wrapWithAdts(const Config& cfg,
                                              const uint8_t* frame,
                                              size_t len);

    static int sampleRateFromIndex(int idx);
};

}  // namespace streaming
