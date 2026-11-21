#pragma once

#include "rtmp/MediaStream.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace streaming {

/**
 * EnhancedRtmp: Enhanced RTMP（veovera/enhanced-rtmp v1/v2）视频/音频标签头解析与构造。
 *
 * 视频（typeId=9）：
 *   legacy:   byte0 = FrameType(4) | CodecId(4)，CodecId=7(AVC)
 *   enhanced: byte0 = IsExHeader(1, 最高位) | FrameType(3) | PacketType(4)
 *             byte1..4 = FourCC（'hvc1'/'avc1'/'av01'/'vp09'）
 *             PacketType=1(CodedFrames) 且 FourCC 为 avc1/hvc1 时，后跟 SI24 composition time
 *   PacketType: 0=SequenceStart 1=CodedFrames 2=SequenceEnd 3=CodedFramesX(无 CTS)
 *               4=Metadata 5=MPEG2TSSequenceStart 6=Multitrack 7=ModEx
 *
 * 音频（typeId=8）：
 *   legacy:   byte0 = SoundFormat(4) | rate/size/type，SoundFormat=10(AAC)
 *   enhanced: byte0 = SoundFormat(4)=9(ExHeader) | AudioPacketType(4)；byte1..4 = FourCC
 *   AudioPacketType: 0=SequenceStart 1=CodedFrames 2=SequenceEnd 4=MultichannelConfig
 *                    5=Multitrack 7=ModEx
 *
 * （原实现把 byte0 低 4 位 == 9 当作扩展头、把 byte5 当作 PacketType，与规范不符，
 *   导致 FFmpeg/OBS 推送的 HEVC 被当作 AVC 处理。）
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

    static constexpr uint8_t kPktSequenceStart = 0;
    static constexpr uint8_t kPktCodedFrames = 1;
    static constexpr uint8_t kPktSequenceEnd = 2;
    static constexpr uint8_t kPktCodedFramesX = 3;

    /// 视频标签是否为扩展头
    static bool isExVideo(uint8_t b0) { return (b0 & 0x80) != 0; }
    /// 音频标签是否为扩展头
    static bool isExAudio(uint8_t b0) { return (b0 >> 4) == 9; }

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

    /// FrameType：1=key,2=inter,3=disposable,4=generated,5=command（legacy/enhanced 通用）
    static int frameType(uint8_t firstByte) { return (firstByte >> 4) & 0x07; }

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
