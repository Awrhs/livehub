#include "rtmp/AvcParser.hpp"
#include "core/Logger.hpp"

#include <cstring>

namespace streaming {

namespace {
constexpr uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};
}

bool AvcParser::isKeyframe(const uint8_t* data, size_t len) {
    if (len < 1) return false;
    // FrameType 高 4 位：1 = keyframe
    return ((data[0] >> 4) & 0x0F) == 1;
}

bool AvcParser::isSequenceHeader(const uint8_t* data, size_t len) {
    if (len < 2) return false;
    return ((data[0] & 0x0F) == 7) && (data[1] == 0);  // AVC + seq header
}

int AvcParser::avcPacketType(const uint8_t* data, size_t len) {
    if (len < 2) return -1;
    return data[1];
}

int AvcParser::naluType(const uint8_t* nalu, size_t len) {
    if (len < 1) return -1;
    return nalu[0] & 0x1F;
}

// AVCDecoderConfigurationRecord 布局（ISO 14496-15）:
//  0      configurationVersion (1)
//  1      AVCProfileIndication
//  2      profile_compatibility
//  3      AVCLevelIndication
//  4      lengthSizeMinusOne (低 2 位)  —— NALU 长度字段字节数-1
//  5      numOfSequenceParameterSets (低 5 位)
//  6..    SPS：每个 2 字节长度 + 数据
//  ..     numOfPictureParameterSets (1)
//  ..     PPS：每个 2 字节长度 + 数据
bool AvcParser::extractSpsPps(const uint8_t* d, size_t len,
                               std::vector<uint8_t>& sps,
                               std::vector<uint8_t>& pps) {
    sps.clear();
    pps.clear();
    if (len < 8) return false;
    uint8_t numSps = d[5] & 0x1F;
    size_t off = 6;
    for (uint8_t i = 0; i < numSps && off + 2 <= len; ++i) {
        uint16_t spsLen = (static_cast<uint16_t>(d[off]) << 8) | d[off + 1];
        off += 2;
        if (off + spsLen > len) return false;
        sps.insert(sps.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
        sps.insert(sps.end(), d + off, d + off + spsLen);
        off += spsLen;
    }
    if (off + 1 > len) return false;
    uint8_t numPps = d[off++];
    for (uint8_t i = 0; i < numPps && off + 2 <= len; ++i) {
        uint16_t ppsLen = (static_cast<uint16_t>(d[off]) << 8) | d[off + 1];
        off += 2;
        if (off + ppsLen > len) return false;
        pps.insert(pps.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
        pps.insert(pps.end(), d + off, d + off + ppsLen);
        off += ppsLen;
    }
    return !sps.empty() && !pps.empty();
}

std::vector<uint8_t> AvcParser::avccToAnnexB(const uint8_t* data,
                                              size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len + (len / 8));
    size_t off = 0;
    while (off + 4 <= len) {
        uint32_t naluLen = (static_cast<uint32_t>(data[off]) << 24) |
                           (static_cast<uint32_t>(data[off + 1]) << 16) |
                           (static_cast<uint32_t>(data[off + 2]) << 8) |
                           static_cast<uint32_t>(data[off + 3]);
        off += 4;
        if (off + naluLen > len) {
            // 长度异常，截断到剩余
            naluLen = static_cast<uint32_t>(len - off);
        }
        out.insert(out.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
        out.insert(out.end(), data + off, data + off + naluLen);
        off += naluLen;
    }
    return out;
}

namespace {

// H.264 Exp-Golomb bit reader
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

private:
    const uint8_t* data_;
    size_t len_;
    size_t bytePos_;
    int bitPos_;
};

}  // namespace

bool AvcParser::parseSpsDimensions(const uint8_t* sps, size_t len, int& width,
                                   int& height) {
    if (len < 4) return false;
    // 跳过 NAL header（1 字节）
    BitReader br(sps + 1, len - 1);
    uint32_t profileIdc = br.readBits(8);
    br.readBits(8);  // constraint flags
    br.readBits(8);  // level_idc
    br.readUe();     // seq_parameter_set_id

    if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 ||
        profileIdc == 244 || profileIdc == 44 || profileIdc == 83 ||
        profileIdc == 86 || profileIdc == 118 || profileIdc == 128 ||
        profileIdc == 138 || profileIdc == 139 || profileIdc == 134) {
        uint32_t chromaFormat = br.readUe();
        if (chromaFormat == 3) br.readBit();  // separate_colour_plane_flag
        br.readUe();  // bit_depth_luma_minus8
        br.readUe();  // bit_depth_chroma_minus8
        br.readBit(); // qpprime_y_zero_transform_bypass
        if (br.readBit()) {  // seq_scaling_matrix_present
            for (int i = 0; i < ((chromaFormat != 3) ? 8 : 12); ++i) {
                if (br.readBit()) {  // seq_scaling_list_present
                    int size = (i < 6) ? 16 : 64;
                    int lastScale = 8, nextScale = 8;
                    for (int j = 0; j < size; ++j) {
                        if (nextScale != 0) {
                            int delta = br.readSe();
                            nextScale = (lastScale + delta + 256) % 256;
                        }
                        lastScale = (nextScale == 0) ? lastScale : nextScale;
                    }
                }
            }
        }
    }
    br.readUe();  // log2_max_frame_num_minus4
    uint32_t picOrderCntType = br.readUe();
    if (picOrderCntType == 0) {
        br.readUe();  // log2_max_pic_order_cnt_lsb_minus4
    } else if (picOrderCntType == 1) {
        br.readBit();  // delta_pic_order_always_zero_flag
        br.readSe();   // offset_for_non_ref_pic
        br.readSe();   // offset_for_top_to_bottom_field
        uint32_t n = br.readUe();
        for (uint32_t i = 0; i < n; ++i) br.readSe();
    }
    br.readUe();  // max_num_ref_frames
    br.readBit(); // gaps_in_frame_num_value_allowed
    uint32_t picWidthInMbs = br.readUe() + 1;
    uint32_t picHeightInMapUnits = br.readUe() + 1;
    bool frameMbsOnly = br.readBit();
    if (!frameMbsOnly) br.readBit();  // mb_adaptive_frame_field
    br.readBit();  // direct_8x8_inference
    bool frameCropping = br.readBit();
    uint32_t cropL = 0, cropR = 0, cropT = 0, cropB = 0;
    if (frameCropping) {
        cropL = br.readUe();
        cropR = br.readUe();
        cropT = br.readUe();
        cropB = br.readUe();
    }
    width = static_cast<int>(picWidthInMbs * 16 - (cropL + cropR) * 2);
    height = static_cast<int>(
        (2 - static_cast<uint32_t>(frameMbsOnly)) * picHeightInMapUnits * 16 -
        (cropT + cropB) * 2);
    return width > 0 && height > 0;
}

std::vector<uint8_t> AvcParser::buildAvcC(const std::vector<uint8_t>& sps,
                                          const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> out;
    if (sps.empty() || pps.empty()) return out;

    out.push_back(1);  // configurationVersion
    // 从 SPS NAL 头后读 profile/level；SPS[1]=profile_idc, [2]=compat, [3]=level_idc
    uint8_t profileIdc = sps.size() > 1 ? sps[1] : 0x42;
    uint8_t profileCompat = sps.size() > 2 ? sps[2] : 0x00;
    uint8_t levelIdc = sps.size() > 3 ? sps[3] : 0x1E;
    out.push_back(profileIdc);
    out.push_back(profileCompat);
    out.push_back(levelIdc);
    out.push_back(0xFF);  // lengthSizeMinusOne = 3（4 字节长度前缀）+ reserved 111111
    out.push_back(0xE1);  // numOfSPS = 1 + reserved 111
    out.push_back(static_cast<uint8_t>((sps.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    out.insert(out.end(), sps.begin(), sps.end());
    out.push_back(1);  // numOfPPS
    out.push_back(static_cast<uint8_t>((pps.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

}  // namespace streaming
