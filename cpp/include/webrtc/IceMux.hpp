#pragma once

#include "net/EventLoop.hpp"
#include "net/UdpServer.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace streaming {

class IceAgent;

/**
 * IceMux: 所有 WebRTC 会话共用一个 UDP 端口（单端口复用，ICE-TCP/UDP mux 思路）。
 *
 * 分发规则：
 *   - STUN Binding Request：按 USERNAME 属性中的本端 ufrag（"本端:对端"）找到会话；
 *     交给该会话校验 MESSAGE-INTEGRITY，并记录「对端地址 → 会话」映射；
 *   - 其余报文（DTLS / SRTP / SRTCP）：按对端地址找到会话。
 *
 * 部署时只需开放一个 UDP 端口（原实现每个观众占用一个端口，需映射一整段端口范围）。
 * 线程安全：收包在所属 EventLoop；发送可在任意线程（sendto 系统调用本身线程安全）。
 */
class IceMux {
public:
    IceMux(EventLoop* loop, uint16_t port);
    ~IceMux();

    void start();
    uint16_t port() const { return port_; }

    void add(const std::string& localUfrag, IceAgent* agent);
    void remove(IceAgent* agent);
    void sendto(const uint8_t* data, size_t len, const sockaddr_in& peer);

    size_t sessionCount() const;

private:
    void onDatagram(const uint8_t* data, size_t len, const sockaddr_in& peer, Timestamp t);
    static uint64_t addrKey(const sockaddr_in& a) {
        return (uint64_t(a.sin_addr.s_addr) << 16) | a.sin_port;
    }

    uint16_t port_;
    std::unique_ptr<UdpServer> udp_;
    mutable std::recursive_mutex mu_;  // 分发期间持锁，防止会话被并发移除后访问
    std::map<std::string, IceAgent*> byUfrag_;
    std::map<uint64_t, IceAgent*> byAddr_;
};

}  // namespace streaming
