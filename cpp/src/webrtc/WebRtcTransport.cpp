#include "webrtc/WebRtcTransport.hpp"
#include "core/Logger.hpp"
#include "rtmp/AvcParser.hpp"

#include <arpa/inet.h>

#include <cstring>
#include <random>

namespace streaming {

namespace {
uint32_t randomSsrc() {
    static thread_local std::mt19937 rng(
        std::random_device{}());
    return rng();
}

// 把 AVCC（4 字节长度前缀）的 NALU 列表抽出（不含长度前缀）
std::vector<std::vector<uint8_t>> extractAvccNalus(const uint8_t* d,
                                                   size_t len) {
    std::vector<std::vector<uint8_t>> out;
    size_t off = 0;
    while (off + 4 <= len) {
        uint32_t naluLen = (uint32_t(d[off]) << 24) |
                           (uint32_t(d[off + 1]) << 16) |
                           (uint32_t(d[off + 2]) << 8) | uint32_t(d[off + 3]);
        off += 4;
        if (off + naluLen > len) break;
        out.emplace_back(d + off, d + off + naluLen);
        off += naluLen;
    }
    return out;
}
}  // namespace

WebRtcTransport::WebRtcTransport(EventLoop* loop, uint16_t udpPort,
                                 std::string ufrag, std::string pwd,
                                 std::string localFingerprint,
                                 std::shared_ptr<MediaStream> stream)
    : loop_(loop),
      ufrag_(std::move(ufrag)),
      pwd_(std::move(pwd)),
      localFingerprint_(std::move(localFingerprint)),
      stream_(std::move(stream)),
      ssrc_(randomSsrc()),
      audioSsrc_(randomSsrc()),
      audioSeq_(static_cast<uint16_t>(randomSsrc())),
      audioTsBase_(randomSsrc()) {
    ice_ = std::make_unique<IceAgent>(loop, udpPort);
    ice_->setPassword(pwd_);
    ice_->setOnData(
        [this](const uint8_t* d, size_t n, const sockaddr_in& p) {
            onIceData(d, n, p);
        });

    dtls_ = std::make_unique<DtlsTransport>(
        DtlsTransport::Role::kServer, localFingerprint_);
    dtls_->setSendCallback([this](const uint8_t* d, size_t n) {
        if (ice_) ice_->send(d, n);
    });
    dtls_->setReadyCallback(
        [this](const DtlsTransport::SrtpKeys& keys) {
            srtpSend_ = std::make_unique<SrtpSession>();
            if (!srtpSend_->initSend(keys.profile, keys.sendKey, keys.sendSalt))
                LOG_WARN("WebRtcTransport: SRTP init failed (profile %d)", keys.profile);
            dtlsReady_ = true;
            LOG_INFO("WebRtcTransport: DTLS-SRTP ready, ssrc=%u", ssrc_);
            subscribeMedia();
        });

    packetizer_ = std::make_unique<H264RtpPacketizer>(ssrc_, 1200);
    packetizer_->setPayloadType(96);  // H264 动态 PT
    packetizer_->setCallback([this](const uint8_t* d, size_t n) {
        sendSrtp(d, n);
    });
}

WebRtcTransport::~WebRtcTransport() { stop(); }

void WebRtcTransport::start() {
    if (started_.exchange(true)) return;
    ice_->start();
    dtls_->init();
    // 服务端等待 ClientHello，不主动发握手

    // 订阅推迟到 DTLS-SRTP 就绪之后（见 ready 回调）：此前发送的包全部被丢弃，
    // 观众会从 GOP 中间开始且永远拿不到 SPS/PPS（原实现在此立即订阅）
}

void WebRtcTransport::subscribeMedia() {
    auto stream = stream_.lock();
    if (!stream || subId_) return;
    // 预取序列头中的 SPS/PPS，关键帧前内联发送
    auto sh = stream->videoSequenceHeader();
    if (!sh.empty()) cacheParameterSets(sh);
    if (opus_) {
        auto ash = stream->audioSequenceHeader();
        if (ash.size() > 2 && (ash[0] >> 4) == 10) opus_->init(ash.data() + 2, ash.size() - 2);
    }
    subId_ = stream->subscribeWithGop(
        [w = weak_from_this()](const MediaPacket& p) {
            if (auto self = w.lock()) {
                if (p.type == MediaPacket::kVideo) self->onVideoPacket(p);
                else if (p.type == MediaPacket::kAudio) self->onAudioPacket(p);
            }
        });
}

void WebRtcTransport::cacheParameterSets(const std::vector<uint8_t>& seq) {
    // AVCDecoderConfigurationRecord 位于 RTMP 负载第 5 字节起
    if (seq.size() < 11) return;
    size_t off = 5 + 5;
    uint8_t numSps = seq[off++] & 0x1F;
    std::vector<std::vector<uint8_t>> ps;
    for (int i = 0; i < numSps && off + 2 <= seq.size(); ++i) {
        size_t l = (size_t(seq[off]) << 8) | seq[off + 1];
        off += 2;
        if (off + l > seq.size()) return;
        ps.emplace_back(seq.begin() + off, seq.begin() + off + l);
        off += l;
    }
    if (off < seq.size()) {
        uint8_t numPps = seq[off++];
        for (int i = 0; i < numPps && off + 2 <= seq.size(); ++i) {
            size_t l = (size_t(seq[off]) << 8) | seq[off + 1];
            off += 2;
            if (off + l > seq.size()) return;
            ps.emplace_back(seq.begin() + off, seq.begin() + off + l);
            off += l;
        }
    }
    if (!ps.empty()) paramSets_ = std::move(ps);
}

void WebRtcTransport::stop() {
    if (!started_.exchange(false)) return;
    if (auto stream = stream_.lock()) {
        if (subId_) stream->unsubscribe(subId_);
        subId_ = 0;
    }
    if (ice_) ice_->stop();
}

void WebRtcTransport::setVideoPayloadType(uint8_t pt) {
    if (packetizer_) packetizer_->setPayloadType(pt);
}

std::string WebRtcTransport::candidate() const {
    return ice_ ? ice_->hostCandidate("127.0.0.1") : "";
}

void WebRtcTransport::onIceData(const uint8_t* data, size_t len,
                                const sockaddr_in&) {
    if (!dtlsReady_) {
        dtls_->feed(data, len);
        return;
    }
    // 已完成 DTLS：这里是 SRTP(SRTCP)，目前服务器下行，仅记录（可扩展 RR/NACK）
    (void)data;
    (void)len;
}

void WebRtcTransport::sendSrtp(const uint8_t* data, size_t len) {
    if (!dtlsReady_ || !srtpSend_ || !ice_) return;
    uint8_t buf[2048];
    if (len + 32 > sizeof(buf)) return;
    std::memcpy(buf, data, len);
    int outLen = srtpSend_->protectRtp(buf, static_cast<int>(len),
                                       sizeof(buf));
    if (outLen > 0) {
        ice_->send(buf, static_cast<size_t>(outLen));
    }
}

void WebRtcTransport::onVideoPacket(const MediaPacket& pkt) {
    if (pkt.data.size() < 5) return;
    // RTMP 视频负载：byte0 帧类型/编码，byte1 AVCPacketType，byte2-4 CTS，byte5+ AVCC
    uint8_t avcType = pkt.data[1];
    if (avcType == 0) {  // 序列头：仅缓存 SPS/PPS，随下一个关键帧内联发送
        cacheParameterSets(pkt.data);
        return;
    }
    if (avcType != 1) return;
    // 首个关键帧之前的帧无法解码：丢弃
    if (!sentKeyframe_) {
        if (!pkt.isKeyframe || paramSets_.empty()) return;
        sentKeyframe_ = true;
    }
    int32_t cts = (int32_t(uint8_t(pkt.data[2])) << 16) |
                  (int32_t(uint8_t(pkt.data[3])) << 8) |
                  int32_t(uint8_t(pkt.data[4]));
    if (cts & 0x800000) cts |= ~0xFFFFFF;
    auto nalus = extractAvccNalus(pkt.data.data() + 5, pkt.data.size() - 5);
    if (nalus.empty()) return;
    if (pkt.isKeyframe) {
        // 关键帧前内联 SPS/PPS（RTMP 源通常不在帧内携带参数集）
        nalus.insert(nalus.begin(), paramSets_.begin(), paramSets_.end());
    }
    uint32_t dts90 = pkt.timestamp * 90;
    uint32_t pts90 = dts90 + uint32_t(cts) * 90;
    packetizer_->packetize(nalus, pts90, pkt.isKeyframe);
}

void WebRtcTransport::setAudioPayloadType(int pt) {
    if (pt < 0 || !AacToOpus::available()) return;
    audioPt_ = pt;
    opus_ = std::make_unique<AacToOpus>();
    opus_->setCallback([this](const uint8_t* d, size_t n, uint32_t ts) { sendOpus(d, n, ts); });
}

// AAC → Opus（每个观众独立转码；AAC 解码 + Opus 编码开销很小）
void WebRtcTransport::onAudioPacket(const MediaPacket& pkt) {
    if (!opus_ || pkt.data.size() < 3) return;
    const uint8_t* d = pkt.data.data();
    if ((d[0] >> 4) != 10) return;  // 仅 AAC
    if (d[1] == 0) {                // AudioSpecificConfig
        opus_->init(d + 2, pkt.data.size() - 2);
        return;
    }
    if (!sentKeyframe_) return;  // 与视频同时开始，避免先听到声音再出画面
    if (!opus_->ready()) return;
    opus_->feed(d + 2, pkt.data.size() - 2, pkt.timestamp);
}

void WebRtcTransport::sendOpus(const uint8_t* data, size_t len, uint32_t ts48k) {
    RtpHeader h;
    h.payloadType = static_cast<uint8_t>(audioPt_);
    h.seq = audioSeq_++;
    h.timestamp = audioTsBase_ + ts48k;
    h.ssrc = audioSsrc_;
    h.marker = 0;
    auto pkt = RtpPacket::build(h, data, len);
    sendSrtp(pkt.data(), pkt.size());
}

}  // namespace streaming
