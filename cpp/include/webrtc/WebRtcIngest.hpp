#pragma once

#include "rtmp/MediaStream.hpp"
#include "webrtc/DtlsTransport.hpp"
#include "webrtc/H264RtpDepacketizer.hpp"
#include "webrtc/IceAgent.hpp"
#include "webrtc/SrtpSession.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace streaming {

/**
 * WebRtcIngest: 一路 WHIP 推流入站链路。
 *
 * 作为 ICE-lite + DTLS-SRTP 服务端，接收浏览器/客户端 RTP(H264)，
 * 用 H264RtpDepacketizer 重组 NALU 并包装成 RTMP 风格的 AVCC MediaPacket
 * 写入共享的 MediaStream，从而可被 HLS/HTTP-FLV/WHEP 复用。
 */
class WebRtcIngest : public std::enable_shared_from_this<WebRtcIngest> {
public:
    WebRtcIngest(EventLoop* loop, uint16_t udpPort, std::string ufrag,
                 std::string pwd, std::string localFingerprint,
                 std::shared_ptr<MediaStream> stream);
    ~WebRtcIngest();

    void start();
    void stop();

    const std::string& ufrag() const { return ufrag_; }
    const std::string& pwd() const { return pwd_; }
    std::string candidate() const;
    /// 仅处理协商出的 H.264 视频 PT（BUNDLE 下同一传输还可能有音频等其它 RTP）
    void setVideoPayloadType(int pt) { videoPt_ = pt; }
    uint16_t port() const { return ice_ ? ice_->port() : 0; }
    uint32_t ssrc() const { return ssrc_; }

    void setRemoteFingerprint(const std::string& fp) {
        if (dtls_) dtls_->setRemoteFingerprint(fp);
    }

private:
    void onIceData(const uint8_t* data, size_t len, const sockaddr_in& peer);
    void onNalu(const uint8_t* nalu, size_t len, uint32_t rtpTs, bool marker);

    [[maybe_unused]] EventLoop* loop_;
    std::unique_ptr<IceAgent> ice_;
    std::unique_ptr<DtlsTransport> dtls_;
    std::unique_ptr<SrtpSession> srtpRecv_;
    std::unique_ptr<H264RtpDepacketizer> depacketizer_;

    std::string ufrag_, pwd_, localFingerprint_;
    std::weak_ptr<MediaStream> stream_;

    uint32_t ssrc_;
    std::atomic<bool> started_{false};
    std::atomic<bool> dtlsReady_{false};
    uint32_t clockBase_ = 0;
    bool haveBase_ = false;
    [[maybe_unused]] uint32_t lastRtpTs_ = 0;
    int videoPt_ = -1;
    std::vector<uint8_t> sps_, pps_;
    bool configDirty_ = false;
    // 当前帧（AVCC 累积）
    std::vector<uint8_t> frame_;
    uint32_t frameTs_ = 0;
    bool frameKey_ = false;
    void flushFrame(MediaStream& stream);
};

}  // namespace streaming
