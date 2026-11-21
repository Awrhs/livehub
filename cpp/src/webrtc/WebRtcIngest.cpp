#include "webrtc/WebRtcIngest.hpp"
#include "core/Logger.hpp"

#include <arpa/inet.h>

#include <cstring>
#include <random>

namespace streaming {

namespace {
uint32_t randomSsrc() {
    static thread_local std::mt19937 rng(std::random_device{}());
    return rng();
}

bool isSps(const uint8_t* n) { return (*n & 0x1F) == 7; }
bool isPps(const uint8_t* n) { return (*n & 0x1F) == 8; }
bool isIdr(const uint8_t* n) { return (*n & 0x1F) == 5; }
}  // namespace

WebRtcIngest::WebRtcIngest(EventLoop* loop, uint16_t udpPort, std::string ufrag,
                           std::string pwd, std::string localFingerprint,
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
            srtpRecv_ = std::make_unique<SrtpSession>();
            srtpRecv_->initRecv(key, salt);
            dtlsReady_ = true;
        });

    depacketizer_ = std::make_unique<H264RtpDepacketizer>();
    depacketizer_->setCallback(
        [this](const uint8_t* n, size_t l, uint32_t ts, bool m) {
            onNalu(n, l, ts, m);
        });
}

WebRtcIngest::~WebRtcIngest() { stop(); }

void WebRtcIngest::start() {
    if (started_.exchange(true)) return;
    ice_->start();
    dtls_->init();
}

void WebRtcIngest::stop() {
    if (!started_.exchange(false)) return;
    if (ice_) ice_->stop();
}

std::string WebRtcIngest::candidate() const {
    return ice_ ? ice_->hostCandidate("127.0.0.1") : "";
}

void WebRtcIngest::onIceData(const uint8_t* data, size_t len,
                             const sockaddr_in&) {
    if (!dtlsReady_) {
        dtls_->feed(data, len);
        return;
    }
    if (!srtpRecv_) return;
    uint8_t buf[2048];
    if (len > sizeof(buf)) return;
    std::memcpy(buf, data, len);

    // 区分 RTCP（PT 64+）与 RTP：RTP PT 通常 < 64，RTCP 在 200..206
    bool isRtcp = (len >= 2) && (buf[1] & 0x7F) >= 64 &&
                  (buf[1] & 0x7F) <= 223;
    int out;
    if (isRtcp) {
        out = srtpRecv_->unprotectRtcp(buf, static_cast<int>(len));
        // 服务端暂不处理 RR（可扩展关键帧请求）
    } else {
        out = srtpRecv_->unprotectRtp(buf, static_cast<int>(len));
        if (out > 12) {
            int pt = buf[1] & 0x7F;
            if (videoPt_ < 0 || pt == videoPt_)
                depacketizer_->feed(buf, static_cast<size_t>(out));
        }
    }
}

void WebRtcIngest::onNalu(const uint8_t* nalu, size_t len, uint32_t rtpTs,
                          bool marker) {
    (void)marker;
    auto stream = stream_.lock();
    if (!stream || len == 0) return;

    // RTP 90kHz → ms
    if (!haveBase_) {
        clockBase_ = rtpTs;
        haveBase_ = true;
    }
    uint32_t delta = rtpTs - clockBase_;
    uint32_t tsMs = delta / 90;

    // 累积 SPS/PPS 生成 AVCDecoderConfigurationRecord（最小可用版本）。
    if (isSps(nalu)) sps_.assign(nalu, nalu + len);
    if (isPps(nalu)) pps_.assign(nalu, nalu + len);

    MediaPacket pkt;
    pkt.timestamp = tsMs;

    if ((isSps(nalu) || isPps(nalu)) && !sps_.empty() && !pps_.empty()) {
        // 构造 AVCDecoderConfigurationRecord
        pkt.type = MediaPacket::kVideo;
        pkt.isSequenceHeader = true;
        std::vector<uint8_t>& d = pkt.data;
        d.push_back(0x17);  // keyframe + AVC
        d.push_back(0x00);  // AVCPacketType sequence header
        d.push_back(0x00);
        d.push_back(0x00);
        d.push_back(0x00);  // composition time
        d.push_back(0x01);  // version
        d.push_back(sps_[1]);
        d.push_back(sps_[2]);
        d.push_back(sps_[3]);
        d.push_back(0xFF);  // 6 bits reserved(0xFC) | (naluLenSize-1)=3
        d.push_back(0xE1);  // 3 bits reserved(0xE0) | numSPS=1
        d.push_back((sps_.size() >> 8) & 0xFF);
        d.push_back(sps_.size() & 0xFF);
        d.insert(d.end(), sps_.begin(), sps_.end());
        d.push_back(1);
        d.push_back((pps_.size() >> 8) & 0xFF);
        d.push_back(pps_.size() & 0xFF);
        d.insert(d.end(), pps_.begin(), pps_.end());
        stream->onPacket(std::move(pkt));
        return;
    }

    // 普通 NALU → AVCC（4 字节长度前缀）
    pkt.type = MediaPacket::kVideo;
    pkt.isKeyframe = isIdr(nalu);
    pkt.data.push_back(isIdr(nalu) ? 0x17 : 0x27);
    pkt.data.push_back(0x01);  // NALU
    pkt.data.push_back(0x00);
    pkt.data.push_back(0x00);
    pkt.data.push_back(0x00);
    uint32_t l = static_cast<uint32_t>(len);
    pkt.data.push_back((l >> 24) & 0xFF);
    pkt.data.push_back((l >> 16) & 0xFF);
    pkt.data.push_back((l >> 8) & 0xFF);
    pkt.data.push_back(l & 0xFF);
    pkt.data.insert(pkt.data.end(), nalu, nalu + len);
    stream->onPacket(std::move(pkt));
}

}  // namespace streaming
