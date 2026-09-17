#pragma once

#include "core/NonCopyable.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace streaming {

/**
 * Socket: 对 POSIX socket fd 的 RAII 封装。
 *
 * 设计要点：
 * - 析构时自动 close(fd)，避免 fd 泄漏
 * - 提供 SO_REUSEADDR / SO_REUSEPORT / TCP_NODELAY / keepalive
 * - bind / listen / accept 封装
 *
 * 简历亮点：RAII 资源管理，理解 socket 选项与 fd 生命周期
 */
class Socket : NonCopyable {
public:
    explicit Socket(int sockfd = -1) : sockfd_(sockfd) {}

    ~Socket() {
        if (sockfd_ >= 0) {
            ::close(sockfd_);
        }
    }

    // 移动语义
    Socket(Socket&& other) noexcept : sockfd_(other.sockfd_) {
        other.sockfd_ = -1;
    }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (sockfd_ >= 0) ::close(sockfd_);
            sockfd_ = other.sockfd_;
            other.sockfd_ = -1;
        }
        return *this;
    }

    int fd() const { return sockfd_; }

    /// 设置非阻塞
    bool setNonBlocking() {
        int flags = ::fcntl(sockfd_, F_GETFL, 0);
        if (flags < 0) return false;
        return ::fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    bool setReuseAddr(bool on = true) {
        int optval = on ? 1 : 0;
        return ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR,
                            &optval, sizeof(optval)) == 0;
    }

    bool setReusePort(bool on = true) {
        int optval = on ? 1 : 0;
        return ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT,
                            &optval, sizeof(optval)) == 0;
    }

    bool setTcpNoDelay(bool on = true) {
        int optval = on ? 1 : 0;
        return ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY,
                            &optval, sizeof(optval)) == 0;
    }

    bool setKeepAlive(bool on = true) {
        int optval = on ? 1 : 0;
        return ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE,
                            &optval, sizeof(optval)) == 0;
    }

    bool bindAddress(uint16_t port, const std::string& ip = "0.0.0.0") {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        if (ip == "0.0.0.0" || ip.empty()) {
            addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
        } else {
            ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        }
        return ::bind(sockfd_, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) == 0;
    }

    bool listen(int backlog = SOMAXCONN) {
        return ::listen(sockfd_, backlog) == 0;
    }

    int accept(struct sockaddr_in* peeraddr = nullptr) {
        socklen_t addrlen = sizeof(struct sockaddr_in);
        if (peeraddr) {
            return ::accept(sockfd_, reinterpret_cast<struct sockaddr*>(peeraddr),
                             &addrlen);
        }
        return ::accept(sockfd_, nullptr, nullptr);
    }

    void shutdownWrite() {
        if (sockfd_ >= 0) ::shutdown(sockfd_, SHUT_WR);
    }

    /// 创建一个非阻塞 TCP socket
    static int createNonblocking() {
        int sockfd = ::socket(AF_INET,
                              SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              IPPROTO_TCP);
        return sockfd;
    }

    void swap(Socket& other) {
        std::swap(sockfd_, other.sockfd_);
    }

private:
    int sockfd_;
};

}  // namespace streaming
