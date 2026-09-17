#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace streaming {

/**
 * HevcParser: H.265/HEVC NALU 与 HEVCDecoderConfigurationRecord(hvCC) 解析。
 *
 * HEVC NALU header（2 字节）：
 *   forbidden_zero_bit(1) | nal_unit_type(6) | nuh_layer_id(6) | nuh_temporal_id_plus1(3)
 *
 * NAL unit type 取值（重点）：
 *   32 VPS, 33 SPS, 34 PPS, 19 IDR_W_RADL, 20 IDR_N_LP,
 *   21 CRA_NUT, 35/36/37/38/39/40 AUD/EOS/EOB/FD/...
 *
 * HEVCDecoderConfigurationRecord（ISO 14496-15, 8.3.3.1.2）布局：
 *   0   configurationVersion
 *   1   general_profile_space(2)/general_tier_flag(1)/general_profile_idc(5)
 *   2.. general_profile_compatibility_flags(4)
 *   6.. general_constraint_indicator_flags(48bit=6B)
 *   12  general_level_idc
 *   13..14 min_spatial_segmentation_idc(2B)
 *   15  parallelismType (高 2 位 reserved)
 *   16  chromaFormat (高 2 位 reserved)
 *   17  bitDepthLumaMinus8 (高 3 位 reserved)
 *   18  bitDepthChromaMinus8 (高 3 位 reserved)
 *   19..20 avgFrameRate(2B)
 *   21  constantFrameRate(2b), numTemporalLayers(3b), temporalIdNested(1b), lengthSizeMinusOne(2b)
 *   22  numOfArrays
 *   23.. arrays: each {array_completeness(1b)/reserved(1b)/NAL_unit_type(6b)(1B), numNalus(2B),
 *                    {nalUnitLength(2B), nalUnit}* }
 *
 * 简历点：手写 HEVC 配置记录解析、VPS/SPS/PPS 提取、HVCC↔Annex-B 转换。
 */
class HevcParser {
public:
    enum NaluType : uint8_t {
        kNalVps = 32,
        kNalSps = 33,
        kNalPps = 34,
        kNalAud = 35,
        kNalIdrWRadl = 19,
        kNalIdrNLp = 20,
        kNalCraNut = 21,
        kNalRaslR = 9,
        kNalRaslN = 10,
        kNalBlaWLp = 16,
        kNalBlaWRadl = 17,
        kNalBlaNLp = 18,
    };

    /// NALU 类型（第 0 字节的高 6 位右移 1）
    static int naluType(const uint8_t* nalu, size_t len);

    /// 是否关键帧（IDR/CRA/BLA）
    static bool isKeyframeNalu(int naluType);

    /// 判断一个 Annex-B access unit（多 NALU 拼接）是否以关键帧 NALU 开头
    static bool isKeyframe(const uint8_t* data, size_t len);

    /// 从 HEVCDecoderConfigurationRecord 提取 VPS/SPS/PPS（带 00 00 00 01 起始码）
    static bool extractConfig(const uint8_t* hvcc, size_t len,
                              std::vector<uint8_t>& vps,
                              std::vector<uint8_t>& sps,
                              std::vector<uint8_t>& pps);

    /// 把 HVCC 长度前缀数据（4 字节 NALU length）转成 Annex-B
    static std::vector<uint8_t> hvccToAnnexB(const uint8_t* data, size_t len,
                                             uint8_t lengthSizeMinusOne = 3);

    /// 把若干裸 NALU（不含起始码）打包成 HVCC 长度前缀格式
    static std::vector<uint8_t> naluToHvcc(const uint8_t* nalu, size_t len,
                                           uint8_t lengthSizeMinusOne = 3);

    /// 从 SPS（不含起始码）解析宽高。成功返回 true。
    static bool parseSpsDimensions(const uint8_t* sps, size_t len, int& width,
                                   int& height);

    /// 返回 hvCC 中的 lengthSizeMinusOne（默认 3 = 4 字节长度前缀）
    static uint8_t lengthSizeMinusOne(const uint8_t* hvcc, size_t len);

    /// 合并 sequence header 里的 VPS/SPS/PPS 为一段 Annex-B（用于 muxer 首帧）
    static std::vector<uint8_t> buildAnnexBConfig(const uint8_t* hvcc, size_t len);

    /// 从 Annex-B 的 VPS/SPS/PPS NALU（不含起始码）构造 HEVCDecoderConfigurationRecord。
    /// 用于 TS(Annex-B) -> Enhanced RTMP(hvCC) 的转换。
    static std::vector<uint8_t> buildHvccFromAnnexB(
        const std::vector<uint8_t>& vps, const std::vector<uint8_t>& sps,
        const std::vector<uint8_t>& pps);
};

}  // namespace streaming
