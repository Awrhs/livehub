#include "hls/TsDemuxer.hpp"
#include "rtmp/EnhancedRtmp.hpp"

#include "rtmp/AvcParser.hpp"
#include "rtmp/HevcParser.hpp"

#include <cstring>

namespace streaming {

namespace {
constexpr uint8_t kPatPid = 0x0000;

// MPEG-TS stream types
constexpr uint8_t kStreamTypeAacAdts = 0x0F;
constexpr uint8_t kStreamTypeAvc = 0x1B;
constexpr uint8_t kStreamTypeHevc = 0x24;
constexpr uint8_t kStreamTypeMp3 = 0x03;

// 64-bit PTS/DTS 解析（33-bit）
int64_t readPts(const uint8_t* p) {
    return (static_cast<int64_t>(p[0] & 0x0E) << 29) |
           (static_cast<int64_t>(p[1]) << 22) |
           (static_cast<int64_t>(p[2] & 0xFE) << 14) |
           (static_cast<int64_t>(p[3]) << 7) |
           (static_cast<int64_t>(p[4] >> 1));
}

// 由 AAC ADTS 头生成 AudioSpecificConfig (2 字节)
std::vector<uint8_t> adtsToAsc(int sampleRate, int channels, int profile) {
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int srIdx = 4;
    for (int i = 0; i < 13; ++i)
        if (rates[i] == sampleRate) {
            srIdx = i;
            break;
        }
    std::vector<uint8_t> asc(2);
    asc[0] = static_cast<uint8_t>(((profile - 1) & 0x1F) << 3) |
             static_cast<uint8_t>((srIdx & 0x0F) >> 1);
    asc[1] = static_cast<uint8_t>(((srIdx & 0x01) << 7) |
                                  ((channels & 0x0F) << 3));
    return asc;
}
}  // namespace

void TsDemuxer::feed(const uint8_t* data, size_t len) {
    leftover_.insert(leftover_.end(), data, data + len);
    size_t i = 0;
    while (i + 188 <= leftover_.size()) {
        if (leftover_[i] != 0x47) {
            // 重新同步
            ++i;
            continue;
        }
        handlePacket(leftover_.data() + i);
        i += 188;
    }
    leftover_.erase(leftover_.begin(), leftover_.begin() + i);
}

void TsDemuxer::handlePacket(const uint8_t* p) {
    bool pusi = (p[1] & 0x40) != 0;
    uint16_t pid = static_cast<uint16_t>(((p[1] & 0x1F) << 8) | p[2]);
    uint8_t adapt = (p[3] >> 4) & 0x03;
    size_t off = 4;
    if (adapt == 2 || adapt == 3) {
        uint8_t alen = p[off];
        off += 1 + alen;
    }
    // adaptation_field_control：1=仅 payload，2=仅 adaptation，3=两者
    //（原实现写反，丢弃了绝大多数“仅 payload”的包，解复用不出任何帧）
    if (adapt == 0 || adapt == 2) return;
    if (off >= 188) return;

    const uint8_t* payload = p + off;
    size_t payloadLen = 188 - off;

    if (pid == kPatPid) {
        if (!pusi) return;
        size_t po = payload[0] + 1;  // pointer field
        if (po >= payloadLen) return;
        // table_id=0x00 PAT; 跳过 section header(8)
        const uint8_t* sec = payload + po;
        if (payloadLen - po < 8) return;
        size_t secLen = ((sec[1] & 0x0F) << 8) | sec[2];
        size_t end = po + 3 + secLen - 4;  // 不含 CRC
        for (size_t i = po + 8; i + 4 <= end && i < payloadLen; i += 4) {
            uint16_t progNum = (sec[i - po] << 8) | sec[i - po + 1];
            uint16_t nPid = ((sec[i - po + 2] & 0x1F) << 8) | sec[i - po + 3];
            if (progNum != 0) {
                pmtPid_ = nPid;
                pmtKnown_ = true;
            }
        }
        return;
    }

    if (pid == pmtPid_ && pmtKnown_) {
        if (!pusi) return;
        size_t po = payload[0] + 1;
        if (po >= payloadLen) return;
        const uint8_t* sec = payload + po;
        if (payloadLen - po < 12) return;
        uint16_t pcrPid = ((sec[8] & 0x1F) << 8) | sec[9];
        (void)pcrPid;
        uint16_t progInfoLen = ((sec[10] & 0x0F) << 8) | sec[11];
        size_t i = po + 12 + progInfoLen;
        size_t secLen = ((sec[1] & 0x0F) << 8) | sec[2];
        size_t end = po + 3 + secLen - 4;
        while (i + 5 <= end && i < payloadLen) {
            uint8_t st = sec[i - po];
            uint16_t epid = ((sec[i - po + 1] & 0x1F) << 8) | sec[i - po + 2];
            uint16_t esInfoLen = ((sec[i - po + 3] & 0x0F) << 8) | sec[i - po + 4];
            if (epid < 8192) {
                PidState& stt = pids_[epid];
                stt.seen = true;
                stt.streamType = st;
                stt.isVideo = (st == kStreamTypeAvc || st == kStreamTypeHevc);
            }
            i += 5 + esInfoLen;
        }
        return;
    }

    PidState& st = pids_[pid];
    if (!st.seen) return;

    if (pusi) {
        // 先把上一段 PES 输出
        if (!st.pes.empty()) handlePes(st);
        st.pesStart = true;
        st.pts = -1;
        st.dts = -1;
        // PES 包头
        if (payloadLen < 6) return;
        if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01)
            return;
        uint16_t pesLen = (payload[4] << 8) | payload[5];
        uint8_t headerLen = payload[8];
        const uint8_t* ptsField = payload + 9;
        uint8_t ptsDtsFlags = (payload[7] >> 6) & 0x03;
        if (ptsDtsFlags == 0x02 && headerLen >= 5) {
            st.pts = readPts(ptsField);
            st.dts = st.pts;
        } else if (ptsDtsFlags == 0x03 && headerLen >= 10) {
            st.pts = readPts(ptsField);
            st.dts = readPts(ptsField + 5);
        }
        size_t dataOff = 9 + headerLen;
        if (dataOff > payloadLen) return;
        st.pes.assign(payload + dataOff, payload + payloadLen);
        (void)pesLen;
    } else {
        if (!st.pesStart) return;
        st.pes.insert(st.pes.end(), payload, payload + payloadLen);
    }
}

void TsDemuxer::handlePes(PidState& st) {
    if (st.isVideo)
        emitVideo(st);
    else if (st.streamType == kStreamTypeAacAdts || st.streamType == kStreamTypeMp3)
        emitAudio(st);
    st.pes.clear();
}

// 在 Annex-B 缓冲中按 start code 遍历 NALU，分离 VPS/SPS/PPS，
// 其余转成 AVCC/hvCC（4 字节长度前缀）
static std::vector<uint8_t> annexbToConfig(const std::vector<uint8_t>& pes,
                                           bool hevc,
                                           std::vector<uint8_t>& vps,
                                           std::vector<uint8_t>& sps,
                                           std::vector<uint8_t>& pps) {
    std::vector<uint8_t> avcc;
    size_t i = 0;
    const size_t n = pes.size();
    while (i + 3 < n) {
        size_t scLen = 0;
        if (pes[i] == 0 && pes[i + 1] == 0 && pes[i + 2] == 1)
            scLen = 3;
        else if (i + 3 < n && pes[i] == 0 && pes[i + 1] == 0 &&
                 pes[i + 2] == 0 && pes[i + 3] == 1)
            scLen = 4;
        if (scLen == 0) {
            ++i;
            continue;
        }
        size_t start = i + scLen;
        size_t next = start;
        bool found = false;
        while (next + 2 < n) {
            if (pes[next] == 0 && pes[next + 1] == 0 &&
                (pes[next + 2] == 1 ||
                 (next + 3 < n && pes[next + 2] == 0 && pes[next + 3] == 1))) {
                found = true;
                break;
            }
            ++next;
        }
        if (!found) next = n;  // 最后一个 NAL 延伸到缓冲末尾（原实现截掉末尾 2 字节）
        size_t naluLen = next - start;
        if (naluLen > 0) {
            uint8_t nalType;
            if (hevc)
                nalType = (pes[start] >> 1) & 0x3F;
            else
                nalType = pes[start] & 0x1F;

            bool isConfig = false;
            bool isAud = hevc ? (nalType == 35) : (nalType == 9);
            if (isAud) isConfig = true;  // AUD 不进入 AVCC/HVCC 帧数据
            else if (hevc) {
                if (nalType == 32) { vps.assign(pes.begin() + start, pes.begin() + start + naluLen); isConfig = true; }
                else if (nalType == 33) { sps.assign(pes.begin() + start, pes.begin() + start + naluLen); isConfig = true; }
                else if (nalType == 34) { pps.assign(pes.begin() + start, pes.begin() + start + naluLen); isConfig = true; }
            } else {
                if (nalType == 7) { sps.assign(pes.begin() + start, pes.begin() + start + naluLen); isConfig = true; }
                else if (nalType == 8) { pps.assign(pes.begin() + start, pes.begin() + start + naluLen); isConfig = true; }
            }
            if (!isConfig) {
                uint32_t l = static_cast<uint32_t>(naluLen);
                avcc.push_back((l >> 24) & 0xFF);
                avcc.push_back((l >> 16) & 0xFF);
                avcc.push_back((l >> 8) & 0xFF);
                avcc.push_back(l & 0xFF);
                avcc.insert(avcc.end(), pes.begin() + start,
                            pes.begin() + start + naluLen);
            }
        }
        i = next;
    }
    return avcc;
}

void TsDemuxer::flushVideoConfig(PidState& st, bool hevc) {
    if (st.configSent) return;
    if (hevc) {
        if (st.vps.empty() || st.sps.empty() || st.pps.empty()) return;
        auto hvcc = HevcParser::buildHvccFromAnnexB(st.vps, st.sps, st.pps);
        if (hvcc.empty()) return;
        MediaPacket pkt;
        pkt.type = MediaPacket::kVideo;
        pkt.videoCodec = VideoCodec::kHevc;
        pkt.enhanced = true;
        pkt.enhancedPacketType = 0;  // sequence start
        pkt.isSequenceHeader = true;
        pkt.isKeyframe = true;
        pkt.data = EnhancedRtmp::buildVideoMessage(
            EnhancedRtmp::kFourccHvc1, 1, EnhancedRtmp::kPktSequenceStart, 0,
            hvcc.data(), hvcc.size());
        if (cb_) cb_(pkt);
        st.configSent = true;
    } else {
        if (st.sps.empty() || st.pps.empty()) return;
        auto avcc = AvcParser::buildAvcC(st.sps, st.pps);
        if (avcc.empty()) return;
        MediaPacket pkt;
        pkt.type = MediaPacket::kVideo;
        pkt.videoCodec = VideoCodec::kH264;
        pkt.isSequenceHeader = true;
        pkt.isKeyframe = true;
        // legacy: FLV video tag byte (keyframe+AVC=0x17) + AVCPacketType(0 seq)
        pkt.data.push_back(0x17);
        pkt.data.push_back(0x00);
        pkt.data.push_back(0x00);
        pkt.data.push_back(0x00);
        pkt.data.push_back(0x00);
        pkt.data.insert(pkt.data.end(), avcc.begin(), avcc.end());
        if (cb_) cb_(pkt);
        st.configSent = true;
    }
}

uint32_t TsDemuxer::toMs(int64_t ts90) {
    // 音视频共用时间基准（首个带时间戳的 PES），避免两路各自从 0 开始导致不同步
    if (ts90 < 0) return lastMs_;
    if (!haveBase_) {
        haveBase_ = true;
        base90_ = ts90;
    }
    int64_t d = ts90 - base90_;
    if (d < 0) d += (int64_t(1) << 33);  // 33 位 PTS 回绕
    lastMs_ = static_cast<uint32_t>(d / 90);
    return lastMs_;
}

void TsDemuxer::emitVideo(PidState& st) {
    bool hevc = (st.streamType == kStreamTypeHevc);

    // 先把 Annex-B 转为长度前缀并抽 config
    std::vector<uint8_t> sample =
        annexbToConfig(st.pes, hevc, st.vps, st.sps, st.pps);
    flushVideoConfig(st, hevc);
    if (sample.empty()) return;

    uint32_t tsMs = toMs(st.dts >= 0 ? st.dts : st.pts);

    // 判断关键帧（HEVC IDR/CRA；AVC IDR）
    bool keyframe = false;
    for (size_t i = 0; i + 4 < sample.size();) {
        uint32_t l = (uint32_t(sample[i]) << 24) | (uint32_t(sample[i+1]) << 16) |
                     (uint32_t(sample[i+2]) << 8) | sample[i+3];
        if (i + 4 + l > sample.size()) break;
        uint8_t b = sample[i + 4];
        uint8_t nt = hevc ? ((b >> 1) & 0x3F) : (b & 0x1F);
        if (hevc) {
            if (nt == 19 || nt == 20 || nt == 21 || nt == 16 || nt == 17 ||
                nt == 18) {
                keyframe = true;
                break;
            }
        } else {
            if (nt == 5) { keyframe = true; break; }
        }
        i += 4 + l;
    }

    int64_t cts90 = 0;
    if (st.pts >= 0 && st.dts >= 0) cts90 = st.pts - st.dts;

    MediaPacket pkt;
    pkt.type = MediaPacket::kVideo;
    pkt.timestamp = tsMs;
    pkt.isKeyframe = keyframe;
    if (hevc) {
        pkt.videoCodec = VideoCodec::kHevc;
        pkt.enhanced = true;
        pkt.enhancedPacketType = 1;  // coded frames
        // FLV composition time 单位为毫秒（原实现直接写入 90kHz 差值）
        pkt.data = EnhancedRtmp::buildVideoMessage(
            EnhancedRtmp::kFourccHvc1, keyframe ? 1 : 2,
            EnhancedRtmp::kPktCodedFrames, static_cast<int32_t>(cts90 / 90),
            nullptr, 0);
    } else {
        pkt.videoCodec = VideoCodec::kH264;
        pkt.data.push_back(keyframe ? 0x17 : 0x27);
        pkt.data.push_back(0x01);  // AVCPacketType NALU
        int32_t cts = static_cast<int32_t>(cts90 / 90);  // 毫秒
        pkt.data.push_back((cts >> 16) & 0xFF);
        pkt.data.push_back((cts >> 8) & 0xFF);
        pkt.data.push_back(cts & 0xFF);
    }
    pkt.data.insert(pkt.data.end(), sample.begin(), sample.end());
    if (cb_) cb_(pkt);
}

void TsDemuxer::flushAudioConfig(PidState& st) {
    if (st.ascSent || st.asc.empty()) return;
    MediaPacket pkt;
    pkt.type = MediaPacket::kAudio;
    pkt.audioCodec = AudioCodec::kAac;
    pkt.isSequenceHeader = true;
    pkt.data.push_back(0xAF);  // AAC, 44100, stereo
    pkt.data.push_back(0x00);  // AAC sequence header
    pkt.data.insert(pkt.data.end(), st.asc.begin(), st.asc.end());
    if (cb_) cb_(pkt);
    st.ascSent = true;
}

void TsDemuxer::emitAudio(PidState& st) {
    if (st.streamType == kStreamTypeMp3) {
        // MP3 透传：FLV soundFormat=2
        MediaPacket pkt;
        pkt.type = MediaPacket::kAudio;
        pkt.audioCodec = AudioCodec::kMp3;
        pkt.data.push_back(0x2A);  // MP3, 44100, stereo, 16-bit
        pkt.data.insert(pkt.data.end(), st.pes.begin(), st.pes.end());
        pkt.timestamp = toMs(st.pts);
        if (cb_) cb_(pkt);
        return;
    }

    // AAC ADTS：逐帧切分（ADTS sync 0xFFF）；同一 PES 内第 k 帧时间戳 = PTS + k*1024 采样
    size_t i = 0;
    int frameIdx = 0;
    const auto& pes = st.pes;
    while (i + 7 < pes.size()) {
        if ((pes[i] & 0xFF) != 0xFF || (pes[i + 1] & 0xF0) != 0xF0) {
            ++i;
            continue;
        }
        int profile = (pes[i + 2] >> 6) & 0x03;       // 0=AAC Main,1=LC
        int srIdx = (pes[i + 2] >> 2) & 0x0F;
        int channels = ((pes[i + 2] & 0x01) << 2) | ((pes[i + 3] >> 6) & 0x03);
        int frameLen = ((pes[i + 3] & 0x03) << 11) | (pes[i + 4] << 3) |
                       ((pes[i + 5] >> 5) & 0x07);
        static const int rates[] = {96000,88200,64000,48000,44100,32000,24000,
                                    22050,16000,12000,11025,8000,7350};
        int sampleRate = (srIdx < 13) ? rates[srIdx] : 44100;

        if (st.asc.empty()) {
            st.sampleRate = sampleRate;
            st.channels = channels ? channels : 2;
            st.asc = adtsToAsc(sampleRate, st.channels, profile + 1);
            flushAudioConfig(st);
        }

        int hdrLen = (pes[i + 1] & 0x01) ? 7 : 9;  // protection_absent=0 时带 2 字节 CRC
        if (frameLen > hdrLen && i + frameLen <= pes.size()) {
            MediaPacket pkt;
            pkt.type = MediaPacket::kAudio;
            pkt.audioCodec = AudioCodec::kAac;
            pkt.data.push_back(0xAF);
            pkt.data.push_back(0x01);  // AAC raw
            pkt.data.insert(pkt.data.end(), pes.begin() + i + hdrLen,
                            pes.begin() + i + frameLen);
            if (st.pts >= 0) {
                int64_t pts = st.pts + static_cast<int64_t>(frameIdx) * 1024 * 90000 / sampleRate;
                pkt.timestamp = toMs(pts);
            }
            if (cb_) cb_(pkt);
            ++frameIdx;
            i += frameLen;
        } else {
            break;
        }
    }
}

}  // namespace streaming
