#pragma once

#include "net/EventLoop.hpp"
#include "net/UdpServer.hpp"
#include "webrtc/Stun.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace streaming {

class IceMux;

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
    /// 单端口复用模式：不占用独立端口，由 IceMux 按本端 ufrag 分发报文
    IceAgent(EventLoop* loop, IceMux* mux, std::string localUfrag);
    ~IceAgent();

    void setOnCandidate(CandidateCallback cb) { onCandidate_ = std::move(cb); }
    void setOnData(DataCallback cb) { onData_ = std::move(cb); }

    void start();
    /// 最近一次收到对端数据（含 STUN 心跳）的时间（steady clock 毫秒）
    int64_t lastRecvMs() const { return lastRecvMs_.load(); }
    static int64_t steadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    void stop();

    /// 设置 STUN short-term 密码（ice-pwd），用于校验 MESSAGE-INTEGRITY
    void setPassword(const std::string& pwd) { password_ = pwd; }

    /// 服务器反射 / host candidate（candidate: 行）
    std::string hostCandidate(const std::string& publicIp) const;
    uint16_t port() const;

    /// 向已选定对端发送（RTP/RTCP/DTLS）
    void send(const uint8_t* data, size_t len);

    EventLoop* loop() const { return loop_; }

    /// 供 IceMux 调用：STUN 连通性检查（合法返回 true）与其他报文
    bool acceptStun(const uint8_t* data, size_t len, const sockaddr_in& peer);
    void onMuxDatagram(const uint8_t* data, size_t len, const sockaddr_in& peer, Timestamp t);

private:
    void sendRaw(const uint8_t* data, size_t len, const sockaddr_in& peer);
    void onDatagram(const uint8_t* data, size_t len, const sockaddr_in& peer,
                    Timestamp);

    EventLoop* loop_;
    std::unique_ptr<UdpServer> udp_;
    IceMux* mux_ = nullptr;
    std::string ufrag_;
    std::string password_;
    bool selected_ = false;
    std::atomic<int64_t> lastRecvMs_{0};
    sockaddr_in peer_{};
    [[maybe_unused]] uint32_t localIpv4_ = 0;  // 候选地址（host order）
    CandidateCallback onCandidate_;
    DataCallback onData_;
};

}  // namespace streaming
