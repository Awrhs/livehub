#include "net/UdpServer.hpp"
#include "core/Logger.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace streaming {

UdpServer::UdpServer(EventLoop* loop, uint16_t port, const std::string& ip)
    : loop_(loop), port_(port), ip_(ip) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        LOG_FATAL("UdpServer: socket() failed: %s", std::strerror(errno));
    }
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (ip_.empty() || ip_ == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        ::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);
    }
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_FATAL("UdpServer: bind() port %d failed: %s", port_,
                  std::strerror(errno));
    }

    socklen_t alen = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
    port_ = ntohs(addr.sin_port);

    channel_ = std::make_unique<Channel>(loop_, fd_);
    channel_->setReadCallback([this](Timestamp t) { handleRead(t); });
}

UdpServer::~UdpServer() {
    if (channel_) {
        channel_->disableAll();
        channel_->remove();
    }
    if (fd_ >= 0) ::close(fd_);
}

void UdpServer::start() {
    channel_->enableReading();
    LOG_INFO("UdpServer listening on UDP %d", port_);
}

void UdpServer::handleRead(Timestamp t) {
    char buf[2048];
    while (true) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&peer), &plen);
        if (n > 0) {
            if (messageCb_) {
                messageCb_(reinterpret_cast<const uint8_t*>(buf),
                           static_cast<size_t>(n), peer, t);
            }
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            break;
        }
    }
}

void UdpServer::sendto(const void* data, size_t len, const sockaddr_in& peer) {
    auto buf = std::string(static_cast<const char*>(data), len);
    auto p = peer;
    loop_->runInLoop([this, buf, p] {
        ::sendto(fd_, buf.data(), buf.size(), MSG_NOSIGNAL,
                 reinterpret_cast<const sockaddr*>(&p), sizeof(p));
    });
}

}  // namespace streaming
