#include "dvr/Mp4BoxWriter.hpp"

#include <cstring>

namespace streaming {

size_t Mp4BoxWriter::startBox(const char* type) {
    size_t off = buf_.size();
    u32(0);           // size 占位
    fourcc(type);
    return off;
}

void Mp4BoxWriter::endBox(size_t sizeOffset) {
    uint32_t sz = static_cast<uint32_t>(buf_.size() - sizeOffset);
    buf_[sizeOffset] = (sz >> 24) & 0xFF;
    buf_[sizeOffset + 1] = (sz >> 16) & 0xFF;
    buf_[sizeOffset + 2] = (sz >> 8) & 0xFF;
    buf_[sizeOffset + 3] = sz & 0xFF;
}

size_t Mp4BoxWriter::startFullBox(const char* type, uint8_t version,
                                  uint32_t flags) {
    size_t off = startBox(type);
    u8(version);
    u8((flags >> 16) & 0xFF);
    u8((flags >> 8) & 0xFF);
    u8(flags & 0xFF);
    return off;
}

void Mp4BoxWriter::writeFtyp(const std::string& majorBrand,
                             uint32_t minorVersion,
                             const std::vector<std::string>& compat) {
    size_t off = startBox("ftyp");
    append(majorBrand);
    u32(minorVersion);
    for (auto& b : compat) append(b);
    endBox(off);
}

void Mp4BoxWriter::writeMvhd(uint32_t timescale, uint64_t duration) {
    size_t off = startFullBox("mvhd", 0, 0);
    u32(0);                    // creation time
    u32(0);                    // modification time
    u32(timescale);
    u32(static_cast<uint32_t>(duration));
    u32(0x00010000);           // rate 1.0
    u16(0x0100);               // volume 1.0
    u16(0);                    // reserved
    u32(0); u32(0);            // reserved
    // unity matrix
    u32(0x00010000); u32(0); u32(0);
    u32(0); u32(0x00010000); u32(0);
    u32(0); u32(0); u32(0x40000000);
    for (int i = 0; i < 6; ++i) u32(0);  // pre_defined
    u32(2);                    // next track id
    endFullBox(off);
}

void Mp4BoxWriter::writeTkhd(uint32_t trackId, uint64_t duration,
                             uint32_t width, uint32_t height, bool isVideo) {
    size_t off = startFullBox("tkhd", 0, 0x000007);  // enabled/in_movie/in_preview
    u32(0); u32(0);            // creation/modification
    u32(trackId);
    u32(0);                    // reserved
    u32(static_cast<uint32_t>(duration));
    u32(0); u32(0);            // reserved
    u16(0);                    // layer
    u16(0);                    // alternate group
    u16(isVideo ? 0 : 0x0100); // volume
    u16(0);                    // reserved
    u32(0x00010000); u32(0); u32(0);
    u32(0); u32(0x00010000); u32(0);
    u32(0); u32(0); u32(0x40000000);
    u32(width << 16);          // width  16.16
    u32(height << 16);         // height 16.16
    endFullBox(off);
}

void Mp4BoxWriter::writeMdhd(uint32_t timescale, uint64_t duration) {
    size_t off = startFullBox("mdhd", 0, 0);
    u32(0); u32(0);
    u32(timescale);
    u32(static_cast<uint32_t>(duration));
    u16(0x55C4);               // language 'und'
    u16(0);
    endFullBox(off);
}

void Mp4BoxWriter::writeHdlr(const std::string& handlerType,
                             const std::string& name) {
    size_t off = startFullBox("hdlr", 0, 0);
    u32(0);                    // pre_defined
    append(handlerType);
    u32(0); u32(0); u32(0);    // reserved
    append(name);
    u8(0);                     // null terminator
    endFullBox(off);
}

void Mp4BoxWriter::writeVmhd() {
    size_t off = startFullBox("vmhd", 0, 1);
    u16(0);                    // graphicsmode
    u16(0); u16(0); u16(0);    // opcolor
    endFullBox(off);
}

void Mp4BoxWriter::writeSmhd() {
    size_t off = startFullBox("smhd", 0, 0);
    u16(0); u16(0);            // balance/reserved
    endFullBox(off);
}

void Mp4BoxWriter::writeDinf() {
    size_t dinf = startBox("dinf");
    {
        size_t dref = startFullBox("dref", 0, 0);
        u32(1);                // entry count
        size_t url = startFullBox("url ", 0, 1);  // flag 1 = self-contained
        endFullBox(url);
        endFullBox(dref);
    }
    endBox(dinf);
}

void Mp4BoxWriter::writeAvc1(const uint8_t* avcc, size_t avccLen,
                             uint16_t width, uint16_t height) {
    size_t avc1 = startBox("avc1");
    u32(0);                    // reserved
    u16(0);                    // reserved
    u16(1);                    // data_reference_index
    u16(0); u16(0);            // pre_defined / reserved
    u32(0); u32(0); u32(0);    // pre_defined
    u16(width); u16(height);
    u32(0x00480000);           // horiz resolution 72 dpi
    u32(0x00480000);           // vert resolution
    u32(0);                    // reserved
    u16(1);                    // frame count
    // compressorname 32 bytes
    for (int i = 0; i < 32; ++i) u8(0);
    u16(0x0018);               // depth
    u16(0xFFFF);               // pre_defined
    // avcC box（4 字节 size + 'avcC' + AVCDecoderConfigurationRecord）
    u32(static_cast<uint32_t>(8 + avccLen));
    fourcc("avcC");
    append(avcc, avccLen);
    endBox(avc1);
}

void Mp4BoxWriter::writeHvc1(const uint8_t* hvcc, size_t hvccLen,
                             uint16_t width, uint16_t height) {
    size_t hvc1 = startBox("hvc1");
    u32(0);                    // reserved
    u16(0); u16(1);            // reserved / data_reference_index
    u16(0); u16(0);
    u32(0); u32(0); u32(0);    // pre_defined
    u16(width); u16(height);
    u32(0x00480000);           // horiz resolution
    u32(0x00480000);           // vert resolution
    u32(0);
    u16(1);                    // frame count
    for (int i = 0; i < 32; ++i) u8(0);  // compressorname
    u16(0x0018);               // depth
    u16(0xFFFF);
    // hvcC box（size + 'hvcC' + HEVCDecoderConfigurationRecord）
    u32(static_cast<uint32_t>(8 + hvccLen));
    fourcc("hvcC");
    append(hvcc, hvccLen);
    endBox(hvc1);
}

void Mp4BoxWriter::writeMp4a(const uint8_t* asc, size_t ascLen,
                             uint16_t sampleRate, uint8_t channels) {
    size_t mp4a = startBox("mp4a");
    u32(0);                    // reserved
    u16(1);                    // data_reference_index
    u32(0);                    // reserved
    u16(channels);
    u16(16);                   // sample size
    u16(0); u16(0);            // compression id / packet size
    u32(sampleRate << 16);     // sample rate 16.16
    // esds
    {
        size_t esds = startFullBox("esds", 0, 0);
        // ES_DescrTag length = body(5) + DecoderConfigDescr(15+ascLen) + SLConfig(3)
        u8(0x03); u8(23 + static_cast<uint8_t>(ascLen)); u16(0x0000);
        u16(2); u8(0x00);       // ES_ID=2, streamDeps=0
        u8(0x04); u8(15 + static_cast<uint8_t>(ascLen));
        u8(0x40); u8(0x15);     // objectTypeIndication=0x40 (AAC), streamType audio
        u8(0x00); u16(0x0000); u32(0);  // buffer size / max bitrate
        u32(0);                 // avg bitrate
        u8(0x05); u8(static_cast<uint8_t>(ascLen));
        append(asc, ascLen);    // decoder specific config (ASC)
        u8(0x06); u8(0x01); u8(0x02);  // SLConfig
        endFullBox(esds);
    }
    (void)asc; (void)sampleRate;
    endBox(mp4a);
}

void Mp4BoxWriter::writeEmptyStbl(bool video) {
    size_t stbl = startBox("stbl");
    {
        size_t stsd = startFullBox("stsd", 0, 0);
        u32(0);  // entry count filled below; we write 0 entries (fragments carry samples)
        // fMP4 init 通常保留 0 或一个 sample entry；这里写 0 以保持简洁
        endFullBox(stsd);
    }
    {
        size_t stts = startFullBox("stts", 0, 0);
        u32(0);
        endFullBox(stts);
    }
    {
        size_t stsc = startFullBox("stsc", 0, 0);
        u32(0);
        endFullBox(stsc);
    }
    {
        size_t stsz = startFullBox("stsz", 0, 0);
        u32(0); u32(0);
        endFullBox(stsz);
    }
    {
        size_t stco = startFullBox("stco", 0, 0);
        u32(0);
        endFullBox(stco);
    }
    (void)video;
    endBox(stbl);
}

void Mp4BoxWriter::writeVideoTrak(uint32_t trackId, uint32_t timescale,
                                  uint64_t duration,
                                  const std::vector<uint8_t>& decConfig,
                                  uint16_t width, uint16_t height,
                                  bool isHevc) {
    size_t trak = startBox("trak");
    writeTkhd(trackId, duration, width, height, true);
    {
        size_t mdia = startBox("mdia");
        writeMdhd(timescale, duration);
        writeHdlr("vide", "VideoHandler");
        {
            size_t minf = startBox("minf");
            writeVmhd();
            writeDinf();
            {
                size_t stbl = startBox("stbl");
                {
                    size_t stsd = startFullBox("stsd", 0, 0);
                    u32(1);  // one entry
                    if (isHevc)
                        writeHvc1(decConfig.data(), decConfig.size(), width,
                                  height);
                    else
                        writeAvc1(decConfig.data(), decConfig.size(), width,
                                  height);
                    endFullBox(stsd);
                }
                size_t stts = startFullBox("stts", 0, 0); u32(0); endFullBox(stts);
                size_t stsc = startFullBox("stsc", 0, 0); u32(0); endFullBox(stsc);
                size_t stsz = startFullBox("stsz", 0, 0); u32(0); u32(0); endFullBox(stsz);
                size_t stco = startFullBox("stco", 0, 0); u32(0); endFullBox(stco);
                endBox(stbl);
            }
            endBox(minf);
        }
        endBox(mdia);
    }
    endBox(trak);
}

void Mp4BoxWriter::writeAudioTrak(uint32_t trackId, uint32_t timescale,
                                  uint64_t duration,
                                  const std::vector<uint8_t>& asc,
                                  uint16_t sampleRate, uint8_t channels) {
    size_t trak = startBox("trak");
    writeTkhd(trackId, duration, 0, 0, false);
    {
        size_t mdia = startBox("mdia");
        writeMdhd(timescale, duration);
        writeHdlr("soun", "SoundHandler");
        {
            size_t minf = startBox("minf");
            writeSmhd();
            writeDinf();
            {
                size_t stbl = startBox("stbl");
                {
                    size_t stsd = startFullBox("stsd", 0, 0);
                    u32(1);
                    writeMp4a(asc.data(), asc.size(), sampleRate, channels);
                    endFullBox(stsd);
                }
                size_t stts = startFullBox("stts", 0, 0); u32(0); endFullBox(stts);
                size_t stsc = startFullBox("stsc", 0, 0); u32(0); endFullBox(stsc);
                size_t stsz = startFullBox("stsz", 0, 0); u32(0); u32(0); endFullBox(stsz);
                size_t stco = startFullBox("stco", 0, 0); u32(0); endFullBox(stco);
                endBox(stbl);
            }
            endBox(minf);
        }
        endBox(mdia);
    }
    endBox(trak);
}

void Mp4BoxWriter::writeMvex(uint32_t videoTrackId, bool hasAudio,
                             uint32_t audioTrackId) {
    size_t mvex = startBox("mvex");
    {
        size_t trex = startFullBox("trex", 0, 0);
        u32(videoTrackId); u32(1); u32(0); u32(0); u32(0);
        endFullBox(trex);
    }
    if (hasAudio) {
        size_t trex = startFullBox("trex", 0, 0);
        u32(audioTrackId); u32(1); u32(0); u32(0); u32(0);
        endFullBox(trex);
    }
    endBox(mvex);
}

std::vector<uint8_t> Mp4BoxWriter::buildMoov(
    uint32_t videoTimescale, const std::vector<uint8_t>& decConfig,
    uint16_t width, uint16_t height, bool hasAudio, uint32_t audioTimescale,
    const std::vector<uint8_t>& asc, uint16_t sampleRate, uint8_t channels,
    bool isHevc) {
    buf_.clear();
    const uint32_t kVideoTrackId = 1;
    const uint32_t kAudioTrackId = 2;

    size_t moov = startBox("moov");
    writeMvhd(videoTimescale, 0);  // duration 由分片决定，init 写 0
    writeVideoTrak(kVideoTrackId, videoTimescale, 0, decConfig, width, height,
                   isHevc);
    if (hasAudio) {
        writeAudioTrak(kAudioTrackId, audioTimescale, 0, asc, sampleRate,
                       channels);
    }
    writeMvex(kVideoTrackId, hasAudio, kAudioTrackId);
    endBox(moov);
    return take();
}

std::vector<uint8_t> Mp4BoxWriter::buildFragment(
    uint32_t seqNum, uint32_t trackId, uint64_t baseMediaDecodeTime,
    const std::vector<SampleInfo>& samples, const std::vector<uint8_t>& mdat,
    bool video) {
    buf_.clear();

    // ---- moof ----
    size_t moof = startBox("moof");
    {
        size_t mfhd = startFullBox("mfhd", 0, 0);
        u32(seqNum);
        endFullBox(mfhd);
    }
    {
        // traf: tfhd 默认 sample duration/size/flags 从 trex 取
        size_t traf = startBox("traf");
        size_t tfhd = startFullBox("tfhd", 0, 0x020000);  // default-base-is-moof
        u32(trackId);
        endFullBox(tfhd);

        size_t tfdt = startFullBox("tfdt", 1, 0);
        u64(baseMediaDecodeTime);
        endFullBox(tfdt);

        // trun: data offset + 每个 sample 的 duration/size/flags/(cts)
        uint32_t flags = 0x000001;   // data-offset present
        if (!samples.empty()) {
            flags |= 0x000200;       // sample-duration present
            flags |= 0x000400;       // sample-size present
            flags |= 0x000800;       // sample-flags present
            if (video) flags |= 0x001000;  // sample-cts-offsets present
        }
        size_t trun = startFullBox("trun", 1, flags);
        u32(static_cast<uint32_t>(samples.size()));
        // data offset 先占位，moof 写完后回填
        size_t dataOffsetPos = buf_.size();
        u32(0);
        for (auto& s : samples) {
            u32(s.duration);
            u32(s.size);
            // sample flags: 0x02000000 = sample_depends_on=2 (keyframe/idr)
            u32((video && s.isKeyframe) ? 0x02000000u : 0x01010000u);
            if (video) u32(s.ctsOffset);
        }
        endFullBox(trun);
        endBox(traf);
    }
    size_t moofSize = buf_.size() - moof;
    endBox(moof);

    // ---- mdat ----
    size_t mdatBox = startBox("mdat");
    append(mdat);
    endBox(mdatBox);

    // 回填 data offset：从 moof 起点到 mdat payload 的偏移
    uint32_t dataOffset = static_cast<uint32_t>(moofSize + 8 /* mdat header */);
    buf_[dataOffsetPos] = (dataOffset >> 24) & 0xFF;
    buf_[dataOffsetPos + 1] = (dataOffset >> 16) & 0xFF;
    buf_[dataOffsetPos + 2] = (dataOffset >> 8) & 0xFF;
    buf_[dataOffsetPos + 3] = dataOffset & 0xFF;
    return take();
}

std::vector<uint8_t> Mp4BoxWriter::buildVideoFragment(
    uint32_t seqNum, uint32_t trackId, uint64_t baseMediaDecodeTime,
    const std::vector<SampleInfo>& samples, const std::vector<uint8_t>& mdat) {
    return buildFragment(seqNum, trackId, baseMediaDecodeTime, samples, mdat,
                         true);
}

std::vector<uint8_t> Mp4BoxWriter::buildAudioFragment(
    uint32_t seqNum, uint32_t trackId, uint64_t baseMediaDecodeTime,
    const std::vector<SampleInfo>& samples, const std::vector<uint8_t>& mdat) {
    return buildFragment(seqNum, trackId, baseMediaDecodeTime, samples, mdat,
                         false);
}

}  // namespace streaming
