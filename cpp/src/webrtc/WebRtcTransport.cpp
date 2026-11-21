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
      ssrc_(randomSsrc()) {
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
        [this](const std::vector<uint8_t>& key,
               const std::vector<uint8_t>& salt) {
            srtpSend_ = std::make_unique<SrtpSession>();
            srtpSend_->initSend(key, salt);
            dtlsReady_ = true;
            LOG_INFO("WebRtcTransport: DTLS-SRTP ready, ssrc=%u", ssrc_);
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

    auto stream = stream_.lock();
    if (stream) {
        // 先发 sequence header（SPS/PPS）+ GOP，再订阅实时
        // GOP 以 RTMP 顺序回放；这里仅做实时订阅，关键帧由发布端 GOP 触发
        subId_ = stream->subscribe(
            [w = weak_from_this()](const MediaPacket& p) {
                if (auto self = w.lock()) {
                    if (p.type == MediaPacket::kVideo)
                        self->onVideoPacket(p);
                    else if (p.type == MediaPacket::kAudio)
                        self->onAudioPacket(p);
                }
            });
    }
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
    // RTMP video: byte0 = frame type/codec, byte1=AVCPacketType,
    //             byte2-3=cts, byte4+ = AVCC
    uint8_t avcType = pkt.data[1];
    if (avcType == 0) {
        // sequence header：解析出 SPS/PPS 单独打包（NALU 不含长度前缀）
        auto seq = pkt.data;
        // AVCDecoderConfigurationRecord: 5字节头后 numOfSPS/sps...
        size_t off = 5;
        if (off >= seq.size()) return;
        ++off;  // version
        ++off;  // profile
        ++off;  // compat
        ++off;  // level
        uint8_t naluLenSize = (seq[off++] & 0x03) + 1;
        (void)naluLenSize;
        uint8_t numSps = seq[off++] & 0x1F;
        std::vector<std::vector<uint8_t>> nalus;
        for (int s = 0; s < numSps && off + 2 <= seq.size(); ++s) {
            uint16_t spsLen = (uint16_t(seq[off]) << 8) | seq[off + 1];
            off += 2;
            if (off + spsLen > seq.size()) break;
            nalus.emplace_back(seq.data() + off, seq.data() + off + spsLen);
            off += spsLen;
        }
        if (off < seq.size()) {
            uint8_t numPps = seq[off++];
            for (int p = 0; p < numPps && off + 2 <= seq.size(); ++p) {
                uint16_t ppsLen = (uint16_t(seq[off]) << 8) | seq[off + 1];
                off += 2;
                if (off + ppsLen > seq.size()) break;
                nalus.emplace_back(seq.data() + off, seq.data() + off + ppsLen);
                off += ppsLen;
            }
        }
        if (!nalus.empty()) {
            uint32_t ts90 = pkt.timestamp * 90;
            packetizer_->packetize(nalus, ts90, true);
        }
        return;
    }
    if (avcType != 1) return;  // 2=end of sequence, 忽略

    int32_t cts = (int32_t(uint8_t(pkt.data[2])) << 16) |
                  (int32_t(uint8_t(pkt.data[3])) << 8) |
                  int32_t(uint8_t(pkt.data[4]));
    // 24-bit 有符号扩展
    if (cts & 0x800000) cts |= ~0xFFFFFF;
    const uint8_t* avcc = pkt.data.data() + 5;
    size_t avccLen = pkt.data.size() - 5;
    auto nalus = extractAvccNalus(avcc, avccLen);
    if (nalus.empty()) return;
    uint32_t dts90 = pkt.timestamp * 90;
    uint32_t pts90 = dts90 + uint32_t(cts) * 90;
    packetizer_->packetize(nalus, pts90, pkt.isKeyframe);
}

void WebRtcTransport::onAudioPacket(const MediaPacket&) {
    // WebRTC 音频需 Opus；当前源为 AAC，暂不转发（需转码）。
    // 留作扩展：Transcoder 输出 Opus 后在此打包 RTP。
}

}  // namespace streaming
