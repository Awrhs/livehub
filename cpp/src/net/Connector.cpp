#include "net/Connector.hpp"
#include "net/EventLoop.hpp"
#include "net/Socket.hpp"
#include "core/Logger.hpp"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace streaming {

const int Connector::kMaxRetryDelayMs = 30 * 1000;
const int Connector::kInitRetryDelayMs = 500;

Connector::Connector(EventLoop* loop, const std::string& ip, uint16_t port)
    : loop_(loop),
      ip_(ip),
      port_(port),
      state_(kDisconnected),
      connect_(false),
      retryDelayMs_(kInitRetryDelayMs) {
    ::memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_port = ::htons(port_);
    if (ip == "0.0.0.0" || ip.empty()) {
        serverAddr_.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    } else {
        ::inet_pton(AF_INET, ip_.c_str(), &serverAddr_.sin_addr);
    }
}

Connector::~Connector() {
    // channel_ 应为空；若还在连接，stop 后由 stopInLoop 关闭 fd
}

void Connector::start() {
    connect_ = true;
    loop_->runInLoop([this] { this->startInLoop(); });
}

void Connector::restart() {
    loop_->assertInLoopThread();
    setState(kDisconnected);
    retryDelayMs_ = kInitRetryDelayMs;
    connect_ = true;
    startInLoop();
}

void Connector::stop() {
    connect_ = false;
    loop_->queueInLoop([this] { this->stopInLoop(); });
}

void Connector::startInLoop() {
    loop_->assertInLoopThread();
    if (connect_) {
        connect();
    }
}

void Connector::stopInLoop() {
    loop_->assertInLoopThread();
    if (state_ == kConnecting) {
        setState(kDisconnected);
        int sockfd = removeAndResetChannel();
        ::close(sockfd);
    }
}

void Connector::connect() {
    int sockfd = Socket::createNonblocking();
    if (sockfd < 0) {
        LOG_ERROR("Connector::connect socket create failed: %s",
                  strerror(errno));
        return;
    }
    int ret = ::connect(sockfd,
                        reinterpret_cast<struct sockaddr*>(&serverAddr_),
                        sizeof(serverAddr_));
    int savedErrno = ret == 0 ? 0 : errno;
    switch (savedErrno) {
        case 0:
        case EINPROGRESS:
        case EINTR:
        case EISCONN:
            connecting(sockfd);
            break;
        case EAGAIN:
        case EADDRINUSE:
        case EADDRNOTAVAIL:
        case ECONNREFUSED:
        case ENETUNREACH:
        case ETIMEDOUT:
            retry(sockfd);
            break;
        case EACCES:
        case EPERM:
        case EAFNOSUPPORT:
        case EALREADY:
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
            LOG_ERROR("Connector::connect error errno=%d: %s",
                      savedErrno, strerror(savedErrno));
            ::close(sockfd);
            break;
        default:
            LOG_ERROR("Connector::connect unexpected errno=%d: %s",
                      savedErrno, strerror(savedErrno));
            ::close(sockfd);
            break;
    }
}

void Connector::connecting(int sockfd) {
    setState(kConnecting);
    channel_.reset(new Channel(loop_, sockfd));
    channel_->setWriteCallback([this] { this->handleWrite(); });
    channel_->setErrorCallback([this] { this->handleError(); });
    channel_->enableWriting();
}

void Connector::handleWrite() {
    if (state_ != kConnecting) {
        // 可能已被 stop
        return;
    }

    int sockfd = removeAndResetChannel();
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
        err = errno;
    }

    if (err != 0) {
        LOG_WARN("Connector::handleWrite SO_ERROR=%d: %s",
                 err, strerror(err));
        retry(sockfd);
    } else {
        // 确认对端没关闭（连上后立即对端 RST 的情况）
        setState(kConnected);
        if (connect_) {
            if (newConnectionCallback_) {
                newConnectionCallback_(sockfd);
            } else {
                ::close(sockfd);
            }
        } else {
            ::close(sockfd);
        }
    }
}

void Connector::handleError() {
    if (state_ != kConnecting) return;

    int sockfd = removeAndResetChannel();
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
        err = errno;
    }
    LOG_ERROR("Connector::handleError errno=%d: %s", err, strerror(err));
    retry(sockfd);
}

void Connector::retry(int sockfd) {
    ::close(sockfd);
    setState(kDisconnected);
    if (connect_) {
        LOG_INFO("Connector retry connecting to %s:%u in %d ms",
                 ip_.c_str(), port_, retryDelayMs_);
        auto self = shared_from_this();
        retryTimerId_ = loop_->runAfter(
            retryDelayMs_ / 1000.0, [self] { self->startInLoop(); });
        retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);
    }
}

int Connector::removeAndResetChannel() {
    channel_->disableAll();
    channel_->remove();
    int sockfd = channel_->fd();
    // 不能在这里 reset channel_，因为正处于 Channel::handleEvent 中
    loop_->queueInLoop([this] { this->resetChannel(); });
    return sockfd;
}

void Connector::resetChannel() {
    channel_.reset();
}

}  // namespace streaming
