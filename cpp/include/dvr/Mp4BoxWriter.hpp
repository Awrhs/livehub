#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace streaming {

/**
 * Mp4BoxWriter: ISO-BMFF (ISO 14496-12) box 写入器。
 *
 * 用于生成 fragmented MP4（fMP4）以及普通 MP4：
 *   ftyp / moov(trak/mdia/minf/stbl/avcC/esds) / moof(traf/tfhd/trun) / mdat
 *
 * 设计：以 std::vector<uint8_t> 为输出缓冲，大端写入；
 * Box 用 StartBox/EndBox 自动回填 32 位 size（必要时 largesize）。
 *
 * 简历点：手写 ISO-BMFF 容器、H.264 avcC、AAC esds、trun 偏移。
 */
class Mp4BoxWriter {
public:
    Mp4BoxWriter() = default;

    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) {
        buf_.push_back((v >> 8) & 0xFF);
        buf_.push_back(v & 0xFF);
    }
    void u24(uint32_t v) {
        buf_.push_back((v >> 16) & 0xFF);
        buf_.push_back((v >> 8) & 0xFF);
        buf_.push_back(v & 0xFF);
    }
    void u32(uint32_t v) {
        buf_.push_back((v >> 24) & 0xFF);
        buf_.push_back((v >> 16) & 0xFF);
        buf_.push_back((v >> 8) & 0xFF);
        buf_.push_back(v & 0xFF);
    }
    void u64(uint64_t v) {
        for (int i = 7; i >= 0; --i)
            buf_.push_back((v >> (i * 8)) & 0xFF);
    }
    void append(const uint8_t* p, size_t n) {
        buf_.insert(buf_.end(), p, p + n);
    }
    void append(const std::vector<uint8_t>& v) {
        buf_.insert(buf_.end(), v.begin(), v.end());
    }
    void append(const std::string& s) {
        buf_.insert(buf_.end(), s.begin(), s.end());
    }
    /// 写 FourCC type（4 字节）
    void fourcc(const char* t) {
        for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(t[i]));
    }

    /// 开始一个 container box：写入占位 size + type，返回 size 字段偏移
    size_t startBox(const char* type);
    /// 结束 container box：回填 size
    void endBox(size_t sizeOffset);

    /// 写一个 full box（version=0, flags=0）的 size+type+version+flags
    size_t startFullBox(const char* type, uint8_t version = 0,
                        uint32_t flags = 0);
    void endFullBox(size_t sizeOffset) { endBox(sizeOffset); }

    const std::vector<uint8_t>& data() const { return buf_; }
    size_t size() const { return buf_.size(); }
    void clear() { buf_.clear(); }
    std::vector<uint8_t> take() {
        std::vector<uint8_t> out = std::move(buf_);
        buf_.clear();
        return out;
    }

    // ---- 高层 box 构造 ----
    /// ftyp box
    void writeFtyp(const std::string& majorBrand = "iso5",
                   uint32_t minorVersion = 0x200,
                   const std::vector<std::string>& compat = {"iso5",
                                                             "iso6",
                                                             "mp41",
                                                             "avc1"});

    /// mvhd（movie header），timescale/fps
    void writeMvhd(uint32_t timescale, uint64_t duration);

    /// tkhd（track header）
    void writeTkhd(uint32_t trackId, uint64_t duration, uint32_t width,
                   uint32_t height, bool isVideo);

    /// mdhd
    void writeMdhd(uint32_t timescale, uint64_t duration);

    /// hdlr
    void writeHdlr(const std::string& handlerType,
                   const std::string& name = "");

    /// vmhd / smhd
    void writeVmhd();
    void writeSmhd();

    /// dinf/dref（自引用 url ）
    void writeDinf();

    /// stsd/avc1/avcC
    void writeAvc1(const uint8_t* avcc, size_t avccLen, uint16_t width,
                   uint16_t height);

    /// stsd/hvc1/hvcC（HEVC）。hvcc 为 HEVCDecoderConfigurationRecord。
    void writeHvc1(const uint8_t* hvcc, size_t hvccLen, uint16_t width,
                   uint16_t height);

    /// stsd/mp4a/esds（AAC）
    void writeMp4a(const uint8_t* asc, size_t ascLen, uint16_t sampleRate,
                   uint8_t channels);

    /// stbl 占位（fMP4 init 用 stsd/stts/stsc/stsz/stco 空表）
    void writeEmptyStbl(bool video);

    /// video trak（isHevc=false: avc1/avcC；true: hvc1/hvcC）
    void writeVideoTrak(uint32_t trackId, uint32_t timescale, uint64_t duration,
                        const std::vector<uint8_t>& decConfig, uint16_t width,
                        uint16_t height, bool isHevc = false);

    /// audio trak（含 ASC）
    void writeAudioTrak(uint32_t trackId, uint32_t timescale, uint64_t duration,
                        const std::vector<uint8_t>& asc,
                        uint16_t sampleRate, uint8_t channels);

    /// moov 容器（ftyp 之后的 init segment）。isHevc=true 则写 hvc1/hvcC。
    std::vector<uint8_t> buildMoov(uint32_t videoTimescale,
                                   const std::vector<uint8_t>& decConfig,
                                   uint16_t width, uint16_t height,
                                   bool hasAudio, uint32_t audioTimescale,
                                   const std::vector<uint8_t>& asc,
                                   uint16_t sampleRate, uint8_t channels,
                                   bool isHevc = false);

    // ---- fragment 相关 ----
    struct SampleInfo {
        uint32_t duration;  // 以 track timescale 为单位
        uint32_t size;      // 采样字节数（mdat 中的长度）
        uint32_t ctsOffset;  // composition time offset（B 帧用，H264 直播通常 0）
        bool isKeyframe;
    };

    /// 写一个 moof + mdat 分片。baseMediaDecodeTime 单位为 track timescale。
    std::vector<uint8_t> buildVideoFragment(uint32_t seqNum,
                                            uint32_t trackId,
                                            uint64_t baseMediaDecodeTime,
                                            const std::vector<SampleInfo>& samples,
                                            const std::vector<uint8_t>& mdat);

    std::vector<uint8_t> buildAudioFragment(uint32_t seqNum, uint32_t trackId,
                                            uint64_t baseMediaDecodeTime,
                                            const std::vector<SampleInfo>& samples,
                                            const std::vector<uint8_t>& mdat);

private:
    std::vector<uint8_t> buf_;

    void writeMvex(uint32_t videoTrackId, bool hasAudio, uint32_t audioTrackId);
    std::vector<uint8_t> buildFragment(uint32_t seqNum, uint32_t trackId,
                                       uint64_t baseMediaDecodeTime,
                                       const std::vector<SampleInfo>& samples,
                                       const std::vector<uint8_t>& mdat,
                                       bool video);
};

}  // namespace streaming
