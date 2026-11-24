#pragma once

#include "rtmp/MediaStream.hpp"
#include "webrtc/AacToOpus.hpp"
#include "webrtc/DtlsTransport.hpp"
#include "webrtc/IceAgent.hpp"
#include "webrtc/RtpPacket.hpp"
#include "webrtc/Rtcp.hpp"
#include "webrtc/SrtpSession.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

namespace streaming {

class IceMux;

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
                    std::shared_ptr<MediaStream> stream, IceMux* mux = nullptr);
    ~WebRtcTransport();

    void start();
    void stop();

    const std::string& ufrag() const { return ufrag_; }
    const std::string& pwd() const { return pwd_; }
    std::string candidate() const;
    /// 使用 SDP 协商出的 H.264 payload type（原实现固定 96）
    void setVideoPayloadType(uint8_t pt);
    uint16_t port() const { return ice_ ? ice_->port() : 0; }
    uint32_t ssrc() const { return ssrc_; }
    uint32_t audioSsrc() const { return audioSsrc_; }
    /// SDP 协商出 Opus 音频时调用：开启 AAC→Opus 转码并发送音频（pt < 0 表示不发送）
    void setAudioPayloadType(int pt);
    /// 超过 timeoutMs 未收到对端任何数据（浏览器关闭/网络断开）
    bool idle(int64_t timeoutMs) const {
        return ice_ && IceAgent::steadyMs() - ice_->lastRecvMs() > timeoutMs;
    }

    /// 注入对端 SDP 的 fingerprint / setup 角色（DTLS 校验用，可选）
    void setRemoteFingerprint(const std::string& fp) {
        remoteFingerprint_ = fp;
        if (dtls_) dtls_->setRemoteFingerprint(fp);
    }

private:
    void onIceData(const uint8_t* data, size_t len, const sockaddr_in& peer);
    void sendSrtp(const uint8_t* data, size_t len);
    void onVideoPacket(const MediaPacket& pkt);
    void subscribeMedia();
    void cacheParameterSets(const std::vector<uint8_t>& seqHeader);
    std::vector<std::vector<uint8_t>> paramSets_;  // SPS/PPS（发布者线程访问）
    bool sentKeyframe_ = false;
    void onAudioPacket(const MediaPacket& pkt);
    void sendOpus(const uint8_t* data, size_t len, uint32_t ts48k);
    // RTCP Sender Report：音视频 RTP 时间戳都由同一 RTMP 时钟换算而来，
    // 周期性发送 SR（NTP 时间 ↔ 各自 RTP 时间戳）让浏览器做唇音同步
    void maybeSendSenderReports(uint32_t mediaTsMs);
    void sendRtcp(std::vector<uint8_t> pkt);
    int64_t lastSrMs_ = 0;
    uint32_t lastMediaTsMs_ = 0;
    int64_t lastMediaSteadyMs_ = 0;
    uint32_t videoPackets_ = 0, videoOctets_ = 0;
    uint32_t audioPackets_ = 0, audioOctets_ = 0;
    std::unique_ptr<AacToOpus> opus_;
    int audioPt_ = -1;
    uint32_t audioSsrc_ = 0;
    uint16_t audioSeq_ = 0;
    uint32_t audioTsBase_ = 0;

    [[maybe_unused]] EventLoop* loop_;
    std::unique_ptr<IceAgent> ice_;
    std::unique_ptr<DtlsTransport> dtls_;
    std::unique_ptr<SrtpSession> srtpSend_;
    std::unique_ptr<SrtpSession> srtpRecv_;  // 解密观众发来的 SRTCP（NACK / PLI / RR）

    // NACK 重传（RFC 4585）：保存最近发送的视频 RTP 明文，按观众请求以原序号重发
    static constexpr size_t kNackHistory = 512;  // 约 2 秒 @ 2.5Mbps
    struct SentPacket {
        uint16_t seq = 0;
        bool valid = false;
        std::vector<uint8_t> data;
    };
    std::vector<SentPacket> history_;
    std::mutex historyMu_;
    std::mutex sendMu_;  // SRTP 加密上下文非线程安全：发布者线程发送与 UDP 线程重传互斥
    std::atomic<uint64_t> nackRequested_{0};
    std::atomic<uint64_t> retransmitted_{0};
    void rememberVideo(const uint8_t* data, size_t len);
    void onRtcp(const uint8_t* data, size_t len);
    std::unique_ptr<H264RtpPacketizer> packetizer_;

    std::string ufrag_, pwd_, localFingerprint_, remoteFingerprint_;
    std::weak_ptr<MediaStream> stream_;
    MediaStream::SubscriberId subId_ = 0;

    uint32_t ssrc_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<bool> dtlsReady_{false};
};

}  // namespace streaming
