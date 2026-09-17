#pragma once

#include "rtmp/MediaStream.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace streaming {

/**
 * EnhancedRtmp: Enhanced RTMP（veovera/enhanced-rtmp）视频/音频消息头解析。
 *
 * 视频消息（typeId=9）：
 *   legacy:   byte0 = FrameType(4)|CodecId(4)，CodecId=7(AVC) 等
 *   enhanced: byte0 bit0..3 = 0x09（isExHeader 标记），byte0 bit4..7 = FrameType，
 *             byte1..4 = FourCC（'hvc1'/'av01'/'vp09'），byte5 = packet type，
 *             byte6.. 载荷。
 *
 * 包类型（Enhanced Video PacketType）：
 *   0 = SequenceStart（编码配置记录，如 hvCC/av1C/dCoderConfig）
 *   1 = CodedFrames
 *   2 = CodedFramesX（无 CTS）
 *   3 = CodedContext
 *   4 = PostDecoderFormat
 *   5 = AltInfoReference
 *   ...
 *
 * 对于 CodedFrames(pktType=1)，后跟 3 字节 CTS；CodedFramesX 无 CTS。
 *
 * 音频（typeId=8）Enhanced：byte0=0x09 标记，1..3 FourCC('Opus'/'ac-3'等)，
 * byte4 = packet type(0 sequence start / 1 coded frames)，后跟载荷。
 */
class EnhancedRtmp {
public:
    /// FourCC 常量
    static constexpr uint32_t kFourccHvc1 = 0x68766331;  // 'hvc1'
    static constexpr uint32_t kFourccAv01 = 0x61763031;  // 'av01'
    static constexpr uint32_t kFourccVp09 = 0x76703039;  // 'vp09'
    static constexpr uint32_t kFourccAvc1 = 0x61766331;  // 'avc1'
    static constexpr uint32_t kFourccOpus = 0x4f707573;  // 'Opus'
    static constexpr uint32_t kFourccMp4a = 0x6d703461;  // 'mp4a'

    static bool isExHeader(uint8_t firstByte) {
        return (firstByte & 0x0F) == 0x09;
    }

    static uint32_t readFourcc(const uint8_t* d) {
        return (static_cast<uint32_t>(d[0]) << 24) |
               (static_cast<uint32_t>(d[1]) << 16) |
               (static_cast<uint32_t>(d[2]) << 8) |
               static_cast<uint32_t>(d[3]);
    }

    static void writeFourcc(uint32_t fourcc, uint8_t* out) {
        out[0] = (fourcc >> 24) & 0xFF;
        out[1] = (fourcc >> 16) & 0xFF;
        out[2] = (fourcc >> 8) & 0xFF;
        out[3] = fourcc & 0xFF;
    }

    /// FrameType（高 4 位右移 4）：1=key,2=inter,3=disposable,4=generated,5=info
    static int frameType(uint8_t firstByte) { return (firstByte >> 4) & 0x0F; }

    /**
     * 解析 enhanced 视频消息头。返回载荷起点偏移；失败返回 0。
     *  - packetType: 0..3
     *  - cts24: 仅 packetType=1 时有效（3 字节有符号 CTS）
     *  - fourcc: hvc1 等
     *  - payload/payloadLen: 指向编码帧数据
     */
    struct VideoInfo {
        bool enhanced;
        uint32_t fourcc;
        uint8_t packetType;
        int32_t cts;
        int frameType;
        const uint8_t* payload;
        size_t payloadLen;
        VideoCodec codec;
    };

    static bool parseVideo(const uint8_t* data, size_t len, VideoInfo& out);

    /**
     * 解析 enhanced 音频消息头。
     */
    struct AudioInfo {
        bool enhanced;
        uint32_t fourcc;
        uint8_t packetType;  // 0=SequenceStart, 1=CodedFrames
        const uint8_t* payload;
        size_t payloadLen;
        AudioCodec codec;
    };
    static bool parseAudio(const uint8_t* data, size_t len, AudioInfo& out);

    /// 构造一个 enhanced 视频消息（用于回源/转推时重封装）
    /// 返回完整消息字节（含 5 字节 enhanced 头 + CTS + payload）
    static std::vector<uint8_t> buildVideoMessage(uint32_t fourcc,
                                                  uint8_t frameType,
                                                  uint8_t packetType,
                                                  int32_t cts,
                                                  const uint8_t* payload,
                                                  size_t len);

    /// 构造一个 enhanced 音频消息
    static std::vector<uint8_t> buildAudioMessage(uint32_t fourcc,
                                                  uint8_t packetType,
                                                  const uint8_t* payload,
                                                  size_t len);
};

}  // namespace streaming
