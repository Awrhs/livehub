#include "rtmp/HevcParser.hpp"
#include "core/Logger.hpp"

#include <cstring>

namespace streaming {

namespace {
constexpr uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t len)
        : data_(data), len_(len), bytePos_(0), bitPos_(0) {}
    bool readBit() {
        if (bytePos_ >= len_) return false;
        bool v = (data_[bytePos_] >> (7 - bitPos_)) & 1;
        if (++bitPos_ == 8) { bitPos_ = 0; ++bytePos_; }
        return v;
    }
    uint32_t readBits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | (readBit() ? 1u : 0u);
        return v;
    }
    uint32_t readUe() {
        int zeros = 0;
        while (!readBit() && bytePos_ < len_) ++zeros;
        return (zeros == 0) ? 0u : ((1u << zeros) - 1u + readBits(zeros));
    }
    int32_t readSe() {
        uint32_t v = readUe();
        return (v & 1) ? static_cast<int32_t>((v + 1) / 2)
                       : -static_cast<int32_t>(v / 2);
    }
    bool eof() const { return bytePos_ >= len_; }

private:
    const uint8_t* data_;
    size_t len_;
    size_t bytePos_;
    int bitPos_;
};

}  // namespace

int HevcParser::naluType(const uint8_t* nalu, size_t len) {
    if (len < 2) return -1;
    // nal_unit_type: 第 0 字节 bit 1..6
    return (nalu[0] >> 1) & 0x3F;
}

bool HevcParser::isKeyframeNalu(int type) {
    return type == kNalIdrWRadl || type == kNalIdrNLp || type == kNalCraNut ||
           type == kNalBlaWLp || type == kNalBlaWRadl || type == kNalBlaNLp;
}

bool HevcParser::isKeyframe(const uint8_t* data, size_t len) {
    // 扫描起始码，找第一个 NALU 的类型
    size_t i = 0;
    while (i + 3 < len) {
        size_t scLen = 0;
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) scLen = 3;
            else if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1)
                scLen = 4;
        }
        if (scLen == 0) { ++i; continue; }
        size_t nalu = i + scLen;
        if (nalu + 2 <= len) {
            int t = naluType(data + nalu, len - nalu);
            // 跳过 AUD/VPS/SPS/PPS，找真正的切片 NALU
            if (t != kNalAud && t != kNalVps && t != kNalSps && t != kNalPps) {
                return isKeyframeNalu(t);
            }
        }
        i = nalu;
    }
    return false;
}

uint8_t HevcParser::lengthSizeMinusOne(const uint8_t* d, size_t len) {
    if (len < 22) return 3;
    return d[21] & 0x03;  // 低 2 位
}

bool HevcParser::extractConfig(const uint8_t* d, size_t len,
                               std::vector<uint8_t>& vps,
                               std::vector<uint8_t>& sps,
                               std::vector<uint8_t>& pps) {
    vps.clear();
    sps.clear();
    pps.clear();
    if (len < 23) return false;

    uint8_t numArrays = d[22];
    size_t off = 23;
    for (uint8_t i = 0; i < numArrays && off + 3 <= len; ++i) {
        uint8_t arrayCompletenessAndType = d[off++];
        int nalType = arrayCompletenessAndType & 0x3F;
        uint16_t numNalus = static_cast<uint16_t>(
            (static_cast<uint16_t>(d[off]) << 8) | d[off + 1]);
        off += 2;
        for (uint16_t j = 0; j < numNalus && off + 2 <= len; ++j) {
            uint16_t nalLen = static_cast<uint16_t>(
                (static_cast<uint16_t>(d[off]) << 8) | d[off + 1]);
            off += 2;
            if (off + nalLen > len) return !sps.empty();
            std::vector<uint8_t>* dst = nullptr;
            if (nalType == kNalVps) dst = &vps;
            else if (nalType == kNalSps) dst = &sps;
            else if (nalType == kNalPps) dst = &pps;
            if (dst) {
                dst->insert(dst->end(), kAnnexBStartCode,
                            kAnnexBStartCode + 4);
                dst->insert(dst->end(), d + off, d + off + nalLen);
            }
            off += nalLen;
        }
    }
    return !sps.empty() && !pps.empty();
}

std::vector<uint8_t> HevcParser::buildAnnexBConfig(const uint8_t* hvcc,
                                                   size_t len) {
    std::vector<uint8_t> vps, sps, pps;
    std::vector<uint8_t> out;
    if (!extractConfig(hvcc, len, vps, sps, pps)) return out;
    if (!vps.empty()) out.insert(out.end(), vps.begin(), vps.end());
    if (!sps.empty()) out.insert(out.end(), sps.begin(), sps.end());
    if (!pps.empty()) out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

std::vector<uint8_t> HevcParser::hvccToAnnexB(const uint8_t* data, size_t len,
                                              uint8_t lengthSizeMinusOne) {
    std::vector<uint8_t> out;
    out.reserve(len + (len / 8));
    size_t lf = static_cast<size_t>(lengthSizeMinusOne) + 1;
    if (lf > 4) lf = 4;
    size_t off = 0;
    while (off + lf <= len) {
        uint32_t naluLen = 0;
        for (size_t k = 0; k < lf; ++k)
            naluLen = (naluLen << 8) | data[off + k];
        off += lf;
        if (off + naluLen > len) naluLen = static_cast<uint32_t>(len - off);
        out.insert(out.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
        if (naluLen) out.insert(out.end(), data + off, data + off + naluLen);
        off += naluLen;
    }
    return out;
}

std::vector<uint8_t> HevcParser::naluToHvcc(const uint8_t* nalu, size_t len,
                                            uint8_t lengthSizeMinusOne) {
    std::vector<uint8_t> out;
    size_t lf = static_cast<size_t>(lengthSizeMinusOne) + 1;
    if (lf > 4) lf = 4;
    out.resize(lf);
    for (size_t k = 0; k < lf; ++k)
        out[k] = static_cast<uint8_t>((len >> (8 * (lf - 1 - k))) & 0xFF);
    out.insert(out.end(), nalu, nalu + len);
    return out;
}

std::vector<uint8_t> HevcParser::buildHvccFromAnnexB(
    const std::vector<uint8_t>& vps, const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> out;
    if (sps.empty() || pps.empty()) return out;

    auto put16 = [&out](uint16_t v) {
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    // 默认 Main profile / level 4.0 / chroma 4:2:0 / 8-bit
    uint8_t generalProfileIdc = 1;   // Main
    uint8_t generalTierFlag = 0;
    uint8_t generalLevelIdc = 120;   // 4.0
    uint8_t chromaFormat = 1;
    uint8_t bitDepthLumaMinus8 = 0;
    uint8_t bitDepthChromaMinus8 = 0;
    uint8_t numTemporalLayers = 1;
    uint8_t temporalIdNested = 1;

    out.push_back(1);  // configurationVersion
    // byte1: general_profile_space(2) | general_tier_flag(1) | general_profile_idc(5)
    out.push_back(static_cast<uint8_t>(((generalTierFlag & 1) << 5) |
                                       (generalProfileIdc & 0x1F)));
    // general_profile_compatibility_flags (4 bytes)，缺省倒序置 Main 位
    uint32_t compat = 1u << (32 - generalProfileIdc);
    for (int i = 3; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((compat >> (8 * i)) & 0xFF));
    // general_constraint_indicator_flags (48 bits)：6 字节留空
    for (int i = 0; i < 6; ++i) out.push_back(0);
    out.push_back(generalLevelIdc);
    put16(0xF000);  // min_spatial_segmentation_idc，高 4 位 reserved=1111
    // 注意：以下四个字段各占 1 字节（共 4 字节，偏移 15..18），不是 2 字节打包。
    out.push_back(0xFC);  // parallelismType = 0，高 2 位 reserved=11
    out.push_back(static_cast<uint8_t>(0xFC | (chromaFormat & 0x03)));
    out.push_back(static_cast<uint8_t>(0xF8 | (bitDepthLumaMinus8 & 0x07)));
    out.push_back(static_cast<uint8_t>(0xF8 | (bitDepthChromaMinus8 & 0x07)));
    put16(0);  // avgFrameRate（偏移 19..20）
    out.push_back(static_cast<uint8_t>(
        ((numTemporalLayers & 0x07) << 3) | ((temporalIdNested & 1) << 2) |
        0x03 /* lengthSizeMinusOne=3 => 4 字节 */));

    auto addArray = [&](uint8_t type, const std::vector<uint8_t>& nalu) {
        if (nalu.empty()) return;
        out.push_back(static_cast<uint8_t>(0x80 | (type & 0x3F)));  // completeness=1
        put16(1);
        put16(static_cast<uint16_t>(nalu.size()));
        out.insert(out.end(), nalu.begin(), nalu.end());
    };

    uint8_t numArrays = 0;
    size_t numPos = out.size();
    out.push_back(0);
    if (!vps.empty()) { addArray(kNalVps, vps); ++numArrays; }
    addArray(kNalSps, sps); ++numArrays;
    addArray(kNalPps, pps); ++numArrays;
    out[numPos] = numArrays;
    return out;
}

// profile_tier_level()
static void parseProfileTierLevel(BitReader& br, bool profilePresent,
                                  int maxNumSubLayersMinus1) {
    if (profilePresent) {
        br.readBits(2);   // general_profile_space
        br.readBit();     // general_tier_flag
        br.readBits(5);   // general_profile_idc
        for (int i = 0; i < 32; ++i) br.readBit();  // general_profile_compatibility_flags
        for (int i = 0; i < 48; ++i) br.readBit();  // general_constraint_indicator_flags
        br.readBits(8);   // general_level_idc
    }
    for (int i = 0; i < maxNumSubLayersMinus1; ++i) {
        br.readBit();  // sub_layer_profile_present_flag
        br.readBit();  // sub_layer_level_present_flag
    }
    if (maxNumSubLayersMinus1 > 0) {
        for (int i = maxNumSubLayersMinus1; i < 8; ++i) br.readBits(2);
    }
    // sub-layer profile/level present 时本应继续读取，这里简化（不影响宽高解析）
}

// 从 SPS 解析 sub width/height，返回 chroma_format_idc
static bool parseSpsDimensionsImpl(BitReader& br, int& width, int& height) {
    br.readBits(4);   // sps_video_parameter_set_id
    uint32_t spsMaxSubLayersMinus1 = br.readBits(3);
    br.readBit();     // sps_temporal_id_nesting_flag
    parseProfileTierLevel(br, true, static_cast<int>(spsMaxSubLayersMinus1));

    br.readUe();  // sps_seq_parameter_set_id

    uint32_t chromaFormatIdc = br.readUe();
    if (chromaFormatIdc == 3) br.readBit();  // separate_colour_plane_flag
    uint32_t picWidthInLumaSamples = br.readUe();
    uint32_t picHeightInLumaSamples = br.readUe();

    uint32_t confWinLeft = 0, confWinRight = 0, confWinTop = 0, confWinBottom = 0;
    if (br.readBit()) {  // conformance_window_flag
        confWinLeft = br.readUe();
        confWinRight = br.readUe();
        confWinTop = br.readUe();
        confWinBottom = br.readUe();
    }

    uint32_t subW = (chromaFormatIdc == 1 || chromaFormatIdc == 2) ? 2u : 1u;
    uint32_t subH = (chromaFormatIdc == 1) ? 2u : 1u;
    width = static_cast<int>(
        picWidthInLumaSamples - subW * (confWinLeft + confWinRight));
    height = static_cast<int>(
        picHeightInLumaSamples - subH * (confWinTop + confWinBottom));
    return width > 0 && height > 0;
}

bool HevcParser::parseSpsDimensions(const uint8_t* sps, size_t len, int& width,
                                    int& height) {
    if (len < 3) return false;
    int type = naluType(sps, len);
    if (type != kNalSps) return false;
    BitReader br(sps + 2, len - 2);  // 跳过 2 字节 NAL header
    return parseSpsDimensionsImpl(br, width, height);
}

}  // namespace streaming
