#include "hls/TsMuxer.hpp"
#include "hls/PesPacket.hpp"

#include <cstring>

namespace streaming {

TsMuxer::TsMuxer(OutputCallback outputCb)
    : outputCb_(std::move(outputCb)),
      pmtPid_(kDefaultPmtPid),
      pcrPid_(kDefaultVideoPid),
      videoPid_(kDefaultVideoPid),
      audioPid_(kDefaultAudioPid) {}

void TsMuxer::writePatPmt(int64_t pcr) {
    uint8_t pkt[kTsPacketSize];
    TsPacket::buildPat(pkt, pmtPid_, patCc_++ & 0x0F);
    outputCb_(pkt, kTsPacketSize);
    ++packetsWritten_;

    TsPacket::buildPmt(pkt, pmtPid_, pcrPid_, videoPid_, audioPid_,
                        pmtCc_++ & 0x0F, videoStreamType_);
    outputCb_(pkt, kTsPacketSize);
    ++packetsWritten_;
    sincePatPmt_ = 0;
    (void)pcr;
}

void TsMuxer::writePesPackets(uint16_t pid,
                               const std::vector<uint8_t>& pes,
                               int64_t pcr, bool randomAccess) {
    size_t offset = 0;
    uint8_t* cc = nullptr;
    if (pid == videoPid_) cc = &videoCc_;
    else if (pid == audioPid_) cc = &audioCc_;
    else {
        static uint8_t dummy = 0;
        cc = &dummy;
    }

    bool first = true;
    while (offset < pes.size()) {
        uint8_t pkt[kTsPacketSize];
        size_t consumed = 0;
        bool writePcr = first && pid == pcrPid_;
        TsPacket::buildPes(pkt, pid, (*cc)++ & 0x0F, first,
                            pes.data() + offset, pes.size() - offset,
                            writePcr ? pcr : -1,
                            first && randomAccess,
                            &consumed);
        outputCb_(pkt, kTsPacketSize);
        ++packetsWritten_;
        ++sincePatPmt_;
        offset += consumed;
        first = false;

        if (sincePatPmt_ >= kPatPmtInterval) {
            writePatPmt(pcr);
        }
    }
}

void TsMuxer::writeVideo(const uint8_t* data, size_t len, int64_t pts,
                          int64_t dts, bool isKeyframe) {
    if (isKeyframe || sincePatPmt_ >= kPatPmtInterval) {
        writePatPmt(dts);
    }
    auto pes = PesPacket::pack(kPesVideoStreamId, pts, dts, data, len);
    writePesPackets(videoPid_, pes, dts, isKeyframe);
}

void TsMuxer::writeAudio(const uint8_t* data, size_t len, int64_t pts) {
    auto pes = PesPacket::pack(kPesAudioStreamId, pts, data, len);
    writePesPackets(audioPid_, pes, -1, false);
}

}  // namespace streaming
