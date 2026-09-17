#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace streaming {

/**
 * AvcParser: RTMP 视频载荷（AVCPacketType）解析工具。
 *
 * RTMP 中 H.264 视频消息格式：
 *   byte0 = FrameType(4bit)|CodecId(4bit)，1=keyframe, 7=AVC
 *   byte1 = AVCPacketType: 0=sequence header, 1=NALU, 2=end
 *   byte2-4 = composition time
 *   byte5.. = AVCC 数据（4 字节长度前缀的 NALU）
 *
 * 提供 AVCC ↔ Annex-B 转换、SPS/PPS 提取。
 *
 * 简历亮点：手写 H.264 NALU 解析与起始码转换
 */
class AvcParser {
public:
    /// 从视频消息中判断是否关键帧
    static bool isKeyframe(const uint8_t* data, size_t len);

    /// 是否 AVC sequence header
    static bool isSequenceHeader(const uint8_t* data, size_t len);

    /// AVCPacketType：0/1/2，不足返回 -1
    static int avcPacketType(const uint8_t* data, size_t len);

    /// 从 AVCDecoderConfigurationRecord 提取 SPS/PPS
    /// 返回 {sps, pps}（含 start code）
    static bool extractSpsPps(const uint8_t* data, size_t len,
                              std::vector<uint8_t>& sps,
                              std::vector<uint8_t>& pps);

    /// 把 AVCC 数据（4 字节长度前缀）转成 Annex-B（00 00 00 01 前缀）
    /// 返回转换后的数据。若 alreadyAnnexB 则原样拷贝。
    static std::vector<uint8_t> avccToAnnexB(const uint8_t* data, size_t len);

    /// 判断 NALU type（NAL 头的低 5 位）
    static int naluType(const uint8_t* nalu, size_t len);

    /// 从 SPS NALU（不含 start code）解析宽高。成功返回 true。
    static bool parseSpsDimensions(const uint8_t* sps, size_t len, int& width,
                                   int& height);

    /// 从裸 SPS/PPS NALU（不含起始码）构造 AVCDecoderConfigurationRecord。
    /// 用于 TS(Annex-B) -> RTMP(AVCC) 的转换。
    static std::vector<uint8_t> buildAvcC(const std::vector<uint8_t>& sps,
                                          const std::vector<uint8_t>& pps);
};

}  // namespace streaming
