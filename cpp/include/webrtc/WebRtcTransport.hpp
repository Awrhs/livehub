#pragma once

#include "rtmp/MediaStream.hpp"
#include "webrtc/DtlsTransport.hpp"
#include "webrtc/IceAgent.hpp"
#include "webrtc/RtpPacket.hpp"
#include "webrtc/Rtcp.hpp"
#include "webrtc/SrtpSession.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace streaming {

/**
 * WebRtcTransport: 单个 WHEP 观众的 WebRTC 下行链路。
 *
 * ICE-lite + DTLS-SRTP + RTP(H264)，订阅一路 MediaStream，把视频打包为
 * RTP(SRTP 加密) 通过 ICE 选定的 UDP 路径推送。
 *
 * 生命周期：shared_ptr 自持有，订阅回调中用 weak_from_this() 防环；
 * 客户端断开 / DTLS 失败 / 超时后 unsubscribe 并自毁。
 */
class WebRtcTransport
    : public std::enable_shared_from_this<WebRtcTransport> {
public:
    WebRtcTransport(EventLoop* loop, uint16_t udpPort,
                    std::string ufrag, std::string pwd,
                    std::string localFingerprint,
                    std::shared_ptr<MediaStream> stream);
    ~WebRtcTransport();

    void start();
    void stop();

    const std::string& ufrag() const { return ufrag_; }
    const std::string& pwd() const { return pwd_; }
    std::string candidate() const;
    uint16_t port() const { return ice_ ? ice_->port() : 0; }
    uint32_t ssrc() const { return ssrc_; }

    /// 注入对端 SDP 的 fingerprint / setup 角色（DTLS 校验用，可选）
    void setRemoteFingerprint(const std::string& fp) {
        remoteFingerprint_ = fp;
        if (dtls_) dtls_->setRemoteFingerprint(fp);
    }

private:
    void onIceData(const uint8_t* data, size_t len, const sockaddr_in& peer);
    void sendSrtp(const uint8_t* data, size_t len);
    void onVideoPacket(const MediaPacket& pkt);
    void onAudioPacket(const MediaPacket& pkt);

    EventLoop* loop_;
    std::unique_ptr<IceAgent> ice_;
    std::unique_ptr<DtlsTransport> dtls_;
    std::unique_ptr<SrtpSession> srtpSend_;
    std::unique_ptr<H264RtpPacketizer> packetizer_;

    std::string ufrag_, pwd_, localFingerprint_, remoteFingerprint_;
    std::weak_ptr<MediaStream> stream_;
    MediaStream::SubscriberId subId_ = 0;

    uint32_t ssrc_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<bool> dtlsReady_{false};
};

}  // namespace streaming
