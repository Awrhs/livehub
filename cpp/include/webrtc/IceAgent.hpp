#pragma once

#include "net/EventLoop.hpp"
#include "net/UdpServer.hpp"
#include "webrtc/Stun.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace streaming {

/**
 * IceAgent: ICE-lite 代理（RFC 8445）。
 *
 * 每个 WebRTC 传输绑定一个 UDP 端口，收到 STUN Binding Request 时回复
 * XOR-MAPPED-ADDRESS，并通过 onSelectedCandidatePair 锁定对端地址；
 * 锁定后非 STUN 数据交给上层（DTLS / RTP / RTCP）。
 *
 * ICE-lite 只应答 connectivity check，不主动发起（适合媒体服务器）。
 */
class IceAgent {
public:
    using CandidateCallback =
        std::function<void(const sockaddr_in& peer)>;
    using DataCallback =
        std::function<void(const uint8_t* data, size_t len,
                           const sockaddr_in& peer)>;

    IceAgent(EventLoop* loop, uint16_t port);
    ~IceAgent();

    void setOnCandidate(CandidateCallback cb) { onCandidate_ = std::move(cb); }
    void setOnData(DataCallback cb) { onData_ = std::move(cb); }

    void start();
    void stop();

    /// 设置 STUN short-term 密码（ice-pwd），用于校验 MESSAGE-INTEGRITY
    void setPassword(const std::string& pwd) { password_ = pwd; }

    /// 服务器反射 / host candidate（candidate: 行）
    std::string hostCandidate(const std::string& publicIp) const;
    uint16_t port() const { return udp_ ? udp_->port() : 0; }

    /// 向已选定对端发送（RTP/RTCP/DTLS）
    void send(const uint8_t* data, size_t len);

    EventLoop* loop() const { return loop_; }

private:
    void onDatagram(const uint8_t* data, size_t len, const sockaddr_in& peer,
                    Timestamp);

    EventLoop* loop_;
    std::unique_ptr<UdpServer> udp_;
    std::string password_;
    bool selected_ = false;
    sockaddr_in peer_{};
    uint32_t localIpv4_ = 0;  // 候选地址（host order）
    CandidateCallback onCandidate_;
    DataCallback onData_;
};

}  // namespace streaming
