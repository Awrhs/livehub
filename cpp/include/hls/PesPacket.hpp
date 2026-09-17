#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace streaming {

constexpr uint8_t kPesVideoStreamId = 0xE0;  // video stream (xxx0)
constexpr uint8_t kPesAudioStreamId = 0xC0;  // audio stream

/**
 * PesPacket: MPEG-PS/PES 封包（含 PTS/DTS 时间戳）。
 *
 * 输入是编码后的 access unit（AVC Annex-B 帧 / 带 ADTS 的 AAC），
 * 输出 0x000001 起始码开头的 PES 数据，交给 TsMuxer 切片。
 */
class PesPacket {
public:
    /// 把一帧封成 PES。
    /// @param streamId 0xE0 视频 / 0xC0 音频
    /// @param pts 90kHz 单位
    /// @param dts 90kHz 单位；<0 表示与 pts 相同
    /// @param payload 帧数据
    static std::vector<uint8_t> pack(uint8_t streamId, int64_t pts,
                                     int64_t dts,
                                     const uint8_t* payload, size_t len);

    /// 仅 PTS（无 DTS）
    static std::vector<uint8_t> pack(uint8_t streamId, int64_t pts,
                                     const uint8_t* payload, size_t len) {
        return pack(streamId, pts, -1, payload, len);
    }

private:
    static void writeTsField(std::vector<uint8_t>& out, uint8_t prefix,
                             int64_t ts);
};

}  // namespace streaming
