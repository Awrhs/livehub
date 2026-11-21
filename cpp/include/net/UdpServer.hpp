#pragma once

#include "core/NonCopyable.hpp"
#include "net/Channel.hpp"
#include "net/EventLoop.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <functional>
#include <string>
#include <memory>

namespace streaming {

/**
 * UdpServer: 非阻塞 UDP 接收器（供 ICE/STUN/RTP/RTCP 使用）。
 *
 * 用 recvfrom/sendto 收发，回调携带对端地址，可按 (ip,port) 复用 fd。
 */
class UdpServer : NonCopyable {
public:
    using MessageCallback =
        std::function<void(const uint8_t* data, size_t len,
                           const sockaddr_in& peer, Timestamp)>;

    UdpServer(EventLoop* loop, uint16_t port,
              const std::string& ip = "0.0.0.0");
    ~UdpServer();

    void setMessageCallback(MessageCallback cb) { messageCb_ = std::move(cb); }
    void start();

    /// 发送到指定对端（线程安全：通过 runInLoop）
    void sendto(const void* data, size_t len, const sockaddr_in& peer);

    uint16_t port() const { return port_; }
    int fd() const { return fd_; }

private:
    void handleRead(Timestamp t);

    EventLoop* loop_;
    int fd_ = -1;
    uint16_t port_;
    std::string ip_;
    std::unique_ptr<Channel> channel_;
    MessageCallback messageCb_;
};

}  // namespace streaming
