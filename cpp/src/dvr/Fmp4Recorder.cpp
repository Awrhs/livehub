#include "dvr/Fmp4Recorder.hpp"

#include <chrono>
#include <future>

#include "core/AsyncFileWriter.hpp"
#include "core/Logger.hpp"
#include "rtmp/AvcParser.hpp"
#include "rtmp/EnhancedRtmp.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

namespace streaming {

namespace {

void mkdirp(const std::string& path) {
    if (path.empty()) return;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            ::mkdir(cur.c_str(), 0755);
        }
    }
}

}  // namespace

Fmp4Recorder::Fmp4Recorder(std::shared_ptr<MediaStream> stream, Options opt)
    : stream_(std::move(stream)), opt_(std::move(opt)) {
    audioTimescale_ = opt_.audioSampleRate ? opt_.audioSampleRate
                                           : kAudioTimescaleDefault;
    if (opt_.outputDir.empty()) opt_.outputDir = "./dvr";
}

Fmp4Recorder::~Fmp4Recorder() { stop(); }

void Fmp4Recorder::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    if (!opt_.vodMode) mkdirp(opt_.outputDir);
    else mkdirp(opt_.outputDir.substr(0, opt_.outputDir.find_last_of('/')));

    // 先把已有 sequence header/GOP 取来，避免丢失 avcC/hvcC/ASC
    if (stream_) {
        auto vsh = stream_->videoSequenceHeader();
        if (!vsh.empty() && vsh.size() >= 6) {
            std::lock_guard<std::mutex> lk(mu_);
            if (EnhancedRtmp::isExVideo(vsh[0])) {
                EnhancedRtmp::VideoInfo vi;
                if (EnhancedRtmp::parseVideo(vsh.data(), vsh.size(), vi) &&
                    vi.codec == VideoCodec::kHevc && vi.payloadLen > 0) {
                    isHevc_ = true;
                    avcc_.assign(vi.payload, vi.payload + vi.payloadLen);
                }
            } else if (vsh.size() > 5) {
                isHevc_ = false;
                avcc_.assign(vsh.begin() + 5, vsh.end());
            }
        }
        auto ash = stream_->audioSequenceHeader();
        if (!ash.empty() && ash.size() >= 2) {
            std::lock_guard<std::mutex> lk(mu_);
            if (EnhancedRtmp::isExAudio(ash[0])) {
                EnhancedRtmp::AudioInfo ai;
                if (EnhancedRtmp::parseAudio(ash.data(), ash.size(), ai) &&
                    ai.payloadLen > 0) {
                    asc_.assign(ai.payload, ai.payload + ai.payloadLen);
                    hasAudio_ = true;
                }
            } else if ((ash[0] & 0xF0) == 0xA0 /* AAC legacy */ &&
                       ash.size() > 2) {
                asc_.assign(ash.begin() + 2, ash.end());
                hasAudio_ = true;
            }
        }
        // 原子回放 GOP 后订阅：录制从缓存的最近 IDR 开始，而不是从 GOP 中间开始
        subId_ = stream_->subscribeWithGop(
            [w = weak_from_this()](const MediaPacket& pkt) {
                if (auto self = w.lock()) self->onPacket(pkt);
            });
    }
}

void Fmp4Recorder::stop() {
    if (!running_.exchange(false)) return;
    if (stream_ && subId_ != 0) {
        stream_->unsubscribe(subId_);
        subId_ = 0;
    }
    std::lock_guard<std::mutex> lk(mu_);
    flushFragment(true);
    if (opt_.vodMode && asyncWriter_ && vodFileId_ != 0) {
        // 等待异步写盘全部落地后再返回：调用方（stopRecord）随后会做 faststart、
        // 业务平台会移动文件；若写入仍在队列中，读到的是不完整文件，
        // 且后续写入会落到已被改写的文件上，导致录像损坏（trun 找不到 tfhd）。
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        asyncWriter_->close(vodFileId_, [done](bool, uint64_t) { done->set_value(); });
        vodFileId_ = 0;
        if (fut.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
            LOG_WARN("Fmp4Recorder: 等待录像写盘超时: %s", opt_.outputDir.c_str());
    }
}

void Fmp4Recorder::ensureInit() {
    if (inited_ || avcc_.empty()) return;
    auto moov = writer_.buildMoov(kVideoTimescale, avcc_, opt_.width,
                                  opt_.height, hasAudio_ && !asc_.empty(),
                                  audioTimescale_, asc_, opt_.audioSampleRate,
                                  opt_.audioChannels, isHevc_);
    std::vector<uint8_t> out;
    Mp4BoxWriter w;
    if (isHevc_)
        w.writeFtyp("iso5", 0x200,
                    {"iso5", "iso6", "mp41", "hvc1"});
    else
        w.writeFtyp();
    out = w.take();
    out.insert(out.end(), moov.begin(), moov.end());

    if (opt_.vodMode) {
        // 整个录制文件 = ftyp+moov + 后续 moof/mdat
        if (asyncWriter_) {
            vodFileId_ = asyncWriter_->open(opt_.outputDir, /*append=*/false);
            asyncWriter_->append(vodFileId_, out);
        } else {
            writeFile(opt_.outputDir, out);
        }
    } else {
        writeFile(initPath(), out);
    }
    inited_ = true;
}

void Fmp4Recorder::writeFile(const std::string& path,
                             const std::vector<uint8_t>& data) {
    if (asyncWriter_) {
        uint64_t id = asyncWriter_->open(path, /*append=*/false);
        asyncWriter_->append(id, data);
        asyncWriter_->close(id);
        return;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        LOG_ERROR("Fmp4Recorder: open %s failed: %s", path.c_str(),
                  std::strerror(errno));
        return;
    }
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

void Fmp4Recorder::appendFile(const std::string& path,
                              const std::vector<uint8_t>& data) {
    if (asyncWriter_) {
        if (vodFileId_ == 0)
            vodFileId_ = asyncWriter_->open(path, /*append=*/true);
        asyncWriter_->append(vodFileId_, data);
        return;
    }
    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) {
        LOG_ERROR("Fmp4Recorder: append %s failed: %s", path.c_str(),
                  std::strerror(errno));
        return;
    }
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

void Fmp4Recorder::onPacket(const MediaPacket& pkt) {
    if (pkt.type == MediaPacket::kVideo) handleVideo(pkt);
    else if (pkt.type == MediaPacket::kAudio) handleAudio(pkt);
}

bool Fmp4Recorder::acceptVideo(uint32_t dts, bool isKey) {
    // 录像必须从关键帧开始（否则开头一段无法解码）；首个关键帧确定音视频共同时间基准
    // （原实现音视频各自以首帧为 0，起点可相差一个 GOP，导致音画不同步）
    if (!haveBase_) {
        if (!isKey) return false;
        haveBase_ = true;
        baseTs_ = dts;
    }
    return dts >= baseTs_;
}

void Fmp4Recorder::handleVideo(const MediaPacket& pkt) {
    const uint8_t* d = pkt.data.data();
    size_t n = pkt.data.size();
    if (n < 5) return;

    // ---- Enhanced RTMP (H.265/HEVC) ----
    if (pkt.enhanced && pkt.videoCodec == VideoCodec::kHevc) {
        EnhancedRtmp::VideoInfo vi;
        if (!EnhancedRtmp::parseVideo(d, n, vi)) return;
        if (vi.packetType == EnhancedRtmp::kPktSequenceStart) {
            // HEVCDecoderConfigurationRecord（hvCC）
            if (vi.payloadLen == 0) return;
            std::lock_guard<std::mutex> lk(mu_);
            isHevc_ = true;
            avcc_.assign(vi.payload, vi.payload + vi.payloadLen);
            ensureInit();
            return;
        }
        if (vi.packetType != EnhancedRtmp::kPktCodedFrames &&
            vi.packetType != EnhancedRtmp::kPktCodedFramesX)
            return;
        if (vi.payloadLen == 0) return;

        uint32_t dts = pkt.timestamp;
        uint32_t pts = dts + static_cast<uint32_t>(vi.cts);
        const uint8_t* sample = vi.payload;
        size_t sampleLen = vi.payloadLen;

        std::lock_guard<std::mutex> lk(mu_);
        if (avcc_.empty()) return;
        bool isKey = pkt.isKeyframe;
        if (!acceptVideo(dts, isKey)) return;
        ensureInit();
        uint32_t dts90 = (dts - baseTs_) * 90u;
        uint32_t pts90 = pts > baseTs_ ? (pts - baseTs_) * 90u : dts90;
        if (isKey && !videoSamples_.empty() &&
            (dts - baseTs_) - curVideoSegmentMs_ >= opt_.segmentDurationMs) {
            flushFragment(false);
        }
        if (videoSamples_.empty()) {
            videoBaseDts_ = dts90;
            curVideoSegmentMs_ = dts - baseTs_;
        }
        Mp4BoxWriter::SampleInfo s;
        s.duration = 0;
        s.size = static_cast<uint32_t>(sampleLen);
        s.ctsOffset = (pts90 > dts90) ? (pts90 - dts90) : 0;
        s.isKeyframe = isKey;
        videoMdat_.insert(videoMdat_.end(), sample, sample + sampleLen);
        videoSamples_.push_back(s);
        videoDts_.push_back(dts90);
        lastVideoDts_ = dts90;
        return;
    }

    // ---- Legacy AVC / H.264 ----
    int avcType = AvcParser::avcPacketType(d, n);
    if (avcType == 0) {
        // AVCDecoderConfigurationRecord，从字节 5 开始到结尾
        std::lock_guard<std::mutex> lk(mu_);
        isHevc_ = false;
        if (n > 5) avcc_.assign(d + 5, d + n);
        ensureInit();
        return;
    }
    if (avcType != 1) return;  // AVC end / 其它

    // composition time（24-bit signed）
    int32_t cts = (int32_t(d[2]) << 16) | (int32_t(d[3]) << 8) | int32_t(d[4]);
    if (cts & 0x800000) cts |= 0xFF000000;
    uint32_t dts = pkt.timestamp;
    uint32_t pts = dts + static_cast<uint32_t>(cts);

    const uint8_t* sample = d + 5;
    size_t sampleLen = n - 5;

    std::lock_guard<std::mutex> lk(mu_);
    if (avcc_.empty()) return;  // 等 sequence header
    bool isKey = pkt.isKeyframe || AvcParser::isKeyframe(d, n);
    if (!acceptVideo(dts, isKey)) return;
    ensureInit();

    uint32_t dts90 = (dts - baseTs_) * 90u;
    uint32_t pts90 = pts > baseTs_ ? (pts - baseTs_) * 90u : dts90;

    // 到达关键帧且当前分片时长超过目标，则切片（音视频一起 flush）
    if (isKey && !videoSamples_.empty() &&
        (dts - baseTs_) - curVideoSegmentMs_ >= opt_.segmentDurationMs) {
        flushFragment(false);
    }

    if (videoSamples_.empty()) {
        videoBaseDts_ = dts90;
        curVideoSegmentMs_ = dts - baseTs_;
    }

    Mp4BoxWriter::SampleInfo s;
    s.duration = 0;  // flush 时统一回填
    s.size = static_cast<uint32_t>(sampleLen);
    s.ctsOffset = (pts90 > dts90) ? (pts90 - dts90) : 0;
    s.isKeyframe = isKey;
    videoMdat_.insert(videoMdat_.end(), sample, sample + sampleLen);
    videoSamples_.push_back(s);
    videoDts_.push_back(dts90);
    lastVideoDts_ = dts90;
}

void Fmp4Recorder::handleAudio(const MediaPacket& pkt) {
    const uint8_t* d = pkt.data.data();
    size_t n = pkt.data.size();
    if (n < 2) return;
    // AAC: 0xAF.. or 0xA?.. soundFormat=10
    if ((d[0] >> 4) != 10) return;  // 仅处理 AAC
    uint8_t pktType = d[1];
    if (pktType == 0) {
        // sequence header
        if (n > 2) {
            std::lock_guard<std::mutex> lk(mu_);
            asc_.assign(d + 2, d + n);
            hasAudio_ = true;
        }
        return;
    }
    if (pktType != 1) return;
    const uint8_t* sample = d + 2;
    size_t sampleLen = n - 2;

    std::lock_guard<std::mutex> lk(mu_);
    if (asc_.empty()) return;
    if (!haveBase_) {
        // 有视频时以首个视频关键帧为时间基准，之前的音频丢弃；纯音频流以首个音频为基准
        if (!avcc_.empty() || stream_->videoSequenceHeader().size() > 0) return;
        haveBase_ = true;
        baseTs_ = pkt.timestamp;
    }
    if (pkt.timestamp < baseTs_) return;
    ensureInit();
    uint32_t dtsHz =
        static_cast<uint32_t>(uint64_t(pkt.timestamp - baseTs_) *
                              audioTimescale_ / 1000);

    // 音频切片跟随视频关键帧（flushFragment 一起切），不单独触发
    if (audioSamples_.empty()) audioBaseDts_ = dtsHz;

    Mp4BoxWriter::SampleInfo s;
    s.duration = 0;
    s.size = static_cast<uint32_t>(sampleLen);
    s.ctsOffset = 0;
    s.isKeyframe = true;
    audioMdat_.insert(audioMdat_.end(), sample, sample + sampleLen);
    audioSamples_.push_back(s);
    audioDts_.push_back(dtsHz);
    lastAudioDts_ = dtsHz;
}

void Fmp4Recorder::flushFragment(bool force) {
    if (videoSamples_.empty() && audioSamples_.empty()) return;

    // ---- 计算每 sample duration = 下一帧 dts - 当前帧 dts ----
    uint32_t vFallback = kVideoTimescale / 30;
    for (size_t i = 0; i < videoSamples_.size(); ++i) {
        if (i + 1 < videoDts_.size())
            videoSamples_[i].duration =
                static_cast<uint32_t>(videoDts_[i + 1] - videoDts_[i]);
        else
            videoSamples_[i].duration = vFallback;
    }
    uint32_t aFallback = audioTimescale_ / 50;
    for (size_t i = 0; i < audioSamples_.size(); ++i) {
        if (i + 1 < audioDts_.size())
            audioSamples_[i].duration =
                static_cast<uint32_t>(audioDts_[i + 1] - audioDts_[i]);
        else
            audioSamples_[i].duration = aFallback;
    }

    // ---- 构造视频/音频 moof 并合并到一个分片 ----
    // 每个 moof 的 mfhd.sequence_number 必须在整个 presentation 中唯一
    // 且单调递增，所以视频/音频分片各分配一个 fragSeq_。
    std::vector<uint8_t> out;
    uint32_t vFrag = 0;
    if (!videoSamples_.empty()) {
        vFrag = fragSeq_++;
        auto v = writer_.buildVideoFragment(vFrag, 1, videoBaseDts_,
                                            videoSamples_, videoMdat_);
        out.insert(out.end(), v.begin(), v.end());
    }
    uint32_t aFrag = 0;
    if (!audioSamples_.empty()) {
        aFrag = fragSeq_++;
        auto a = writer_.buildAudioFragment(aFrag, 2, audioBaseDts_,
                                            audioSamples_, audioMdat_);
        out.insert(out.end(), a.begin(), a.end());
    }

    if (opt_.vodMode) {
        appendFile(opt_.outputDir, out);
    } else {
        uint32_t seq = segmentSeq_++;
        std::string path =
            opt_.outputDir + "/seg-" + std::to_string(seq) + ".m4s";
        writeFile(path, out);
        segmentSeqs_.push_back(seq);
        if (onSegment_) onSegment_(seq, path);
        pruneOldSegments();
    }
    videoSamples_.clear();
    videoDts_.clear();
    videoMdat_.clear();
    audioSamples_.clear();
    audioDts_.clear();
    audioMdat_.clear();
    (void)force;
}

void Fmp4Recorder::pruneOldSegments() {
    if (opt_.windowCount == 0) return;
    while (segmentSeqs_.size() > opt_.windowCount) {
        uint32_t s = segmentSeqs_.front();
        segmentSeqs_.pop_front();
        std::string path =
            opt_.outputDir + "/seg-" + std::to_string(s) + ".m4s";
        ::unlink(path.c_str());
    }
}

std::vector<std::string> Fmp4Recorder::playlist() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    for (uint32_t s : segmentSeqs_) {
        out.push_back("seg-" + std::to_string(s) + ".m4s");
    }
    return out;
}

}  // namespace streaming
