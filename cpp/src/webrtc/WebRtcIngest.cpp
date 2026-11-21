#include "webrtc/WebRtcIngest.hpp"
#include <chrono>
#include <algorithm>
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
        [this](const DtlsTransport::SrtpKeys& keys) {
            srtpRecv_ = std::make_unique<SrtpSession>();
            if (!srtpRecv_->initRecv(keys.profile, keys.recvKey, keys.recvSalt))
                LOG_WARN("WebRtcIngest: SRTP init failed (profile %d)", keys.profile);
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
    auto stream = stream_.lock();
    if (!stream || len == 0) return;
    if (!haveBase_) {
        clockBase_ = rtpTs;
        haveBase_ = true;
    }
    // 同一 RTP 时间戳的 NALU 属于同一帧：时间戳变化时先输出上一帧
    if (!frame_.empty() && rtpTs != frameTs_) flushFrame(*stream);
    frameTs_ = rtpTs;

    uint8_t type = nalu[0] & 0x1F;
    if (type == 7 || type == 8) {
        // SPS/PPS：内容变化时更新流的序列头（必须 setVideoSequenceHeader，
        // 否则后加入的观众回放 GOP 时拿不到序列头；原实现只分发不保存）
        std::vector<uint8_t>& dst = (type == 7) ? sps_ : pps_;
        if (dst.size() != len || !std::equal(dst.begin(), dst.end(), nalu)) {
            dst.assign(nalu, nalu + len);
            configDirty_ = true;
        }
        if (configDirty_ && !sps_.empty() && !pps_.empty() && sps_.size() >= 4) {
            configDirty_ = false;
            MediaPacket sh;
            sh.type = MediaPacket::kVideo;
            sh.videoCodec = VideoCodec::kH264;
            sh.isSequenceHeader = true;
            sh.isKeyframe = true;
            sh.timestamp = (rtpTs - clockBase_) / 90;
            std::vector<uint8_t>& d = sh.data;
            d = {0x17, 0x00, 0x00, 0x00, 0x00, 0x01, sps_[1], sps_[2], sps_[3], 0xFF, 0xE1};
            d.push_back(static_cast<uint8_t>(sps_.size() >> 8));
            d.push_back(static_cast<uint8_t>(sps_.size()));
            d.insert(d.end(), sps_.begin(), sps_.end());
            d.push_back(1);
            d.push_back(static_cast<uint8_t>(pps_.size() >> 8));
            d.push_back(static_cast<uint8_t>(pps_.size()));
            d.insert(d.end(), pps_.begin(), pps_.end());
            stream->setVideoSequenceHeader(sh.data);
            stream->onPacket(std::move(sh));
        }
    } else if (type != 9) {  // 跳过 AUD；其余 NALU 以 AVCC 形式累积到当前帧
        if (type == 5) frameKey_ = true;
        uint32_t l = static_cast<uint32_t>(len);
        frame_.push_back(static_cast<uint8_t>(l >> 24));
        frame_.push_back(static_cast<uint8_t>(l >> 16));
        frame_.push_back(static_cast<uint8_t>(l >> 8));
        frame_.push_back(static_cast<uint8_t>(l));
        frame_.insert(frame_.end(), nalu, nalu + len);
    }
    if (marker) flushFrame(*stream);  // marker 位表示一帧结束
}

void WebRtcIngest::flushFrame(MediaStream& stream) {
    // 一帧的全部 NALU 放进同一个 FLV/RTMP 视频包（原实现每个 NALU 单独成包，多 slice 帧无法解码）
    if (frame_.empty()) return;
    if (sps_.empty() || pps_.empty()) {  // 还没有参数集：无法解码，丢弃
        frame_.clear();
        frameKey_ = false;
        return;
    }
    MediaPacket pkt;
    pkt.type = MediaPacket::kVideo;
    pkt.videoCodec = VideoCodec::kH264;
    pkt.isKeyframe = frameKey_;
    pkt.timestamp = (frameTs_ - clockBase_) / 90;
    pkt.data.reserve(frame_.size() + 5);
    pkt.data = {static_cast<uint8_t>(frameKey_ ? 0x17 : 0x27), 0x01, 0x00, 0x00, 0x00};
    pkt.data.insert(pkt.data.end(), frame_.begin(), frame_.end());
    frame_.clear();
    frameKey_ = false;
    stream.touch(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count());
    stream.onPacket(std::move(pkt));
}

}  // namespace streaming
