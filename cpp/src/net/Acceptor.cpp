#include "net/Acceptor.hpp"
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
      listening_(false) {
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(true);
    acceptSocket_.bindAddress(port, ip);
    acceptChannel_.setReadCallback(
        [this](Timestamp) { this->handleRead(); });
}

Acceptor::~Acceptor() {
    acceptChannel_.disableAll();
    acceptChannel_.remove();
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
        LOG_ERROR("Acceptor::handleRead accept error: %s", strerror(errno));
        // EMFILE: 文件描述符耗尽，关闭 idle 预留 fd 以腾出空间（简化处理）
        if (errno == EMFILE) {
            LOG_ERROR("too many open files (EMFILE)");
        }
    }
}

}  // namespace streaming
