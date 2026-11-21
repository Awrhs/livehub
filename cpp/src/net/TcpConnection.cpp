#include "net/TcpConnection.hpp"
#include "net/EventLoop.hpp"
#include "net/Channel.hpp"
#include "core/Logger.hpp"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/types.h>

namespace streaming {

TcpConnection::TcpConnection(EventLoop* loop, const std::string& name,
                             int sockfd,
                             const struct sockaddr_in& localAddr,
                             const struct sockaddr_in& peerAddr)
    : loop_(loop),
      name_(name),
      state_(kConnecting),
      socket_(sockfd),
      channel_(loop, sockfd),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024),
      lastReceiveTime_(Timestamp::now()),
      lastActivity_(Timestamp::now()) {
    channel_.setReadCallback(
        [this](Timestamp t) { this->handleRead(t); });
    channel_.setWriteCallback([this] { this->handleWrite(); });
    channel_.setCloseCallback([this] { this->handleClose(); });
    channel_.setErrorCallback([this] { this->handleError(); });
    socket_.setTcpNoDelay(true);
    LOG_DEBUG("TcpConnection ctor [%s] fd=%d", name_.c_str(), sockfd);
}

TcpConnection::~TcpConnection() {
    LOG_DEBUG("TcpConnection dtor [%s] fd=%d state=%d",
              name_.c_str(), socket_.fd(), static_cast<int>(state_.load()));
}

void TcpConnection::connectEstablished() {
    auto self = shared_from_this();
    loop_->runInLoop([this, self] {
        setState(kConnected);
        channel_.tie(self);
        lastActivity_ = Timestamp::now();
        if (idleTimeoutSec_ > 0) {
            // 定时器归连接自身所有，connectDestroyed 时取消
            //（原实现由 TcpServer 为每个连接注册永不取消的 runEvery，定时器随连接数无限累积）
            std::weak_ptr<TcpConnection> weak = self;
            idleTimer_ = loop_->runEvery(1.0, [weak] {
                auto c = weak.lock();
                if (!c || c->state_ != kConnected) return;
                double idle = Timestamp::now() - c->lastActivity_;
                if (idle > c->idleTimeoutSec_) {
                    LOG_WARN("connection [%s] idle %.1fs, force close",
                             c->name_.c_str(), idle);
                    c->forceClose();
                }
            });
        }
        channel_.enableReading();
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
    });
}

void TcpConnection::connectDestroyed() {
    auto self = shared_from_this();
    loop_->runInLoop([this, self] {
        if (idleTimer_) {
            loop_->cancel(idleTimer_);
            idleTimer_ = TimerId();
        }
        if (state_ == kConnected) {
            setState(kDisconnected);
            channel_.disableAll();
            if (connectionCallback_) {
                connectionCallback_(shared_from_this());
            }
        }
        channel_.remove();
    });
}

void TcpConnection::send(const std::string& message) {
    if (state_ == kConnected) {
        if (loop_->isInLoopThread()) {
            sendInLoop(message.data(), message.size());
        } else {
            // 跨线程：拷贝一份加入 loop 队列
            std::string msg = message;  // 显式拷贝
            auto self = shared_from_this();
            queuedBytes_.fetch_add(msg.size(), std::memory_order_relaxed);
            loop_->runInLoop([self, msg] {
                self->queuedBytes_.fetch_sub(msg.size(), std::memory_order_relaxed);
                self->sendInLoop(msg.data(), msg.size());
            });
        }
    }
}

void TcpConnection::send(Buffer* buf) {
    if (state_ == kConnected) {
        if (loop_->isInLoopThread()) {
            sendInLoop(buf->peek(), buf->readableBytes());
            buf->retrieveAll();
        } else {
            std::string data = buf->retrieveAllAsString();
            auto self = shared_from_this();
            queuedBytes_.fetch_add(data.size(), std::memory_order_relaxed);
            loop_->runInLoop([self, data] {
                self->queuedBytes_.fetch_sub(data.size(), std::memory_order_relaxed);
                self->sendInLoop(data.data(), data.size());
            });
        }
    }
}

void TcpConnection::sendInLoop(const void* data, size_t len) {
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    if (state_ == kDisconnected) {
        LOG_WARN("disconnected, give up writing");
        return;
    }

    // 若 output buffer 为空，直接 write，避免注册 EPOLLOUT
    if (!channel_.isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(socket_.fd(), data, len);
        if (nwrote > 0) lastActivity_ = Timestamp::now();
        if (nwrote >= 0) {
            remaining = len - static_cast<size_t>(nwrote);
            if (remaining == 0 && writeCompleteCallback_) {
                auto self = shared_from_this();
                loop_->queueInLoop(
                    [self] { self->writeCompleteCallback_(self); });
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                LOG_ERROR("TcpConnection::sendInLoop write error: %s",
                          strerror(errno));
                if (errno == EPIPE || errno == ECONNRESET) {
                    faultError = true;
                }
            }
        }
    }

    if (!faultError && remaining > 0) {
        size_t oldLen = outputBuffer_.readableBytes();
        outputBuffer_.append(static_cast<const char*>(data) + nwrote, remaining);
        outputBytes_.store(outputBuffer_.readableBytes(), std::memory_order_relaxed);
        if (!channel_.isWriting()) {
            channel_.enableWriting();  // 注册 EPOLLOUT
        }
        // 输出缓冲越过高水位，触发反压回调
        if (oldLen + remaining >= highWaterMark_ &&
            oldLen < highWaterMark_ && highWaterMarkCallback_) {
            auto self = shared_from_this();
            loop_->queueInLoop(
                [self, total = oldLen + remaining] {
                    self->highWaterMarkCallback_(self, total);
                });
        }
    }
}

bool TcpConnection::tcpInfo(TcpConnection::TcpInfo* info) const {
    struct tcp_info tcpi{};
    socklen_t len = sizeof(tcpi);
    if (::getsockopt(socket_.fd(), IPPROTO_TCP, TCP_INFO, &tcpi, &len) == 0) {
        info->state = tcpi.tcpi_state;
        info->rtt = static_cast<uint32_t>(tcpi.tcpi_rtt);
        info->lost = tcpi.tcpi_lost;
        info->retrans = tcpi.tcpi_total_retrans;
        return true;
    }
    return false;
}

void TcpConnection::shutdown() {
    if (state_ == kConnected) {
        setState(kDisconnecting);
        auto self = shared_from_this();
        loop_->runInLoop([self] { self->shutdownInLoop(); });
    }
}

void TcpConnection::shutdownInLoop() {
    if (!channel_.isWriting()) {
        socket_.shutdownWrite();
    }
    // 若还在写，等写完在 handleWrite 中 shutdown
}

void TcpConnection::forceClose() {
    if (state_ == kConnected || state_ == kDisconnecting) {
        setState(kDisconnecting);
        auto self = shared_from_this();
        loop_->queueInLoop([self] { self->forceCloseInLoop(); });
    }
}

void TcpConnection::forceCloseInLoop() {
    if (state_ == kConnected || state_ == kDisconnecting) {
        handleClose();
    }
}

void TcpConnection::handleRead(Timestamp receiveTime) {
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(socket_.fd(), &savedErrno);
    if (n > 0) {
        lastReceiveTime_ = receiveTime;
        lastActivity_ = receiveTime;
        if (messageCallback_) {
            messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
        }
    } else if (n == 0) {
        handleClose();
    } else {
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead read error: %s",
                  strerror(savedErrno));
        handleError();
    }
}

void TcpConnection::handleWrite() {
    if (!channel_.isWriting()) {
        LOG_TRACE("Connection is down, no more writing");
        return;
    }

    if (outputBuffer_.readableBytes() == 0) {
        // 无数据可写却仍关注 EPOLLOUT 会导致 epoll 忙循环
        channel_.disableWriting();
        if (state_ == kDisconnecting) shutdownInLoop();
        return;
    }
    ssize_t n = ::write(socket_.fd(), outputBuffer_.peek(),
                        outputBuffer_.readableBytes());
    if (n > 0) {
        lastActivity_ = Timestamp::now();
        outputBuffer_.retrieve(static_cast<size_t>(n));
        outputBytes_.store(outputBuffer_.readableBytes(), std::memory_order_relaxed);
        if (outputBuffer_.readableBytes() == 0) {
            channel_.disableWriting();
            if (writeCompleteCallback_) {
                auto self = shared_from_this();
                loop_->queueInLoop(
                    [self] { self->writeCompleteCallback_(self); });
            }
            if (state_ == kDisconnecting) {
                shutdownInLoop();
            }
        }
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        // 暂不可写，等待下一次 EPOLLOUT
    } else {
        LOG_WARN("TcpConnection::handleWrite write error: %s, closing [%s]",
                 strerror(errno), name_.c_str());
        handleClose();
    }
}

void TcpConnection::handleClose() {
    setState(kDisconnected);
    channel_.disableAll();

    std::shared_ptr<TcpConnection> guardThis(shared_from_this());
    if (connectionCallback_) {
        connectionCallback_(guardThis);
    }
    if (closeCallback_) {
        closeCallback_(guardThis);
    }
}

void TcpConnection::handleError() {
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (::getsockopt(socket_.fd(), SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
        err = errno;
    }
    LOG_ERROR("TcpConnection [%s] SO_ERROR=%d: %s",
              name_.c_str(), err, strerror(err));
}

}  // namespace streaming
