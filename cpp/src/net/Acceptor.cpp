#include "net/Acceptor.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include "net/EventLoop.hpp"
#include "core/Logger.hpp"

#include <unistd.h>
#include <errno.h>
#include <string.h>

namespace streaming {

Acceptor::Acceptor(EventLoop* loop, uint16_t port, const std::string& ip)
    : loop_(loop),
      acceptSocket_(Socket::createNonblocking()),
      acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false),
      idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
    acceptSocket_.setReuseAddr(true);
    // 不默认开启 SO_REUSEPORT：否则两个实例可同时绑定同一端口、内核悄悄分流，
    // 端口冲突的部署错误不会被发现（源站/边缘同机时 RTSP 8554 即被两进程共享）
    if (!acceptSocket_.bindAddress(port, ip)) {
        // 端口被占用等：立即失败，而不是“假装启动成功”却没有在监听
        LOG_FATAL("bind %s:%u failed: %s", ip.c_str(), port, strerror(errno));
    }
    acceptChannel_.setReadCallback(
        [this](Timestamp) { this->handleRead(); });
}

Acceptor::~Acceptor() {
    acceptChannel_.disableAll();
    acceptChannel_.remove();
    if (idleFd_ >= 0) ::close(idleFd_);
}

void Acceptor::listen() {
    listening_ = true;
    acceptSocket_.listen();
    acceptChannel_.enableReading();
}

void Acceptor::handleRead() {
    struct sockaddr_in peerAddr{};
    int connfd = acceptSocket_.accept(&peerAddr);
    if (connfd >= 0) {
        if (newConnectionCallback_) {
            newConnectionCallback_(connfd, peerAddr);
        } else {
            ::close(connfd);
        }
    } else {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ECONNABORTED)
            return;
        LOG_ERROR("Acceptor::handleRead accept error: %s", strerror(err));
        if (err == EMFILE || err == ENFILE) {
            // fd 耗尽：待处理连接留在队列里会让 epoll 持续可读（忙循环）。
            // 释放预留 fd → 接受并立即关闭该连接 → 重新预留（muduo 惯用法）
            ::close(idleFd_);
            idleFd_ = ::accept(acceptSocket_.fd(), nullptr, nullptr);
            if (idleFd_ >= 0) ::close(idleFd_);
            idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        }
    }
}

}  // namespace streaming
