#include "net/TcpClient.hpp"

#include <algorithm>
#include "net/EventLoop.hpp"
#include "net/Socket.hpp"
#include "core/Logger.hpp"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace streaming {

TcpClient::TcpClient(EventLoop* loop, const std::string& ip, uint16_t port,
                     const std::string& name)
    : loop_(loop),
      connector_(new Connector(loop, ip, port)),
      name_(name),
      retry_(false),
      connect_(true),
      nextConnId_(1) {
    connector_->setNewConnectionCallback(
        [this](int fd) { this->newConnection(fd); });
}

TcpClient::~TcpClient() {
    reconnectAllowed_->store(false);
    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }
    if (conn) {
        // 关闭回调不应再访问 this（避免 use-after-free）
        conn->setCloseCallback([](const TcpConnectionPtr& c) {
            c->shutdown();
        });
    } else {
        connector_->stop();
    }
}

void TcpClient::connect() {
    reconnectAllowed_->store(true);
    connect_ = true;
    connector_->start();
}

void TcpClient::disconnect() {
    connect_ = false;
    reconnectAllowed_->store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            connection_->shutdown();
        }
    }
}

void TcpClient::stop() {
    connect_ = false;
    reconnectAllowed_->store(false);
    connector_->stop();
}

void TcpClient::newConnection(int sockfd) {
    connectedAt_ = Timestamp::now();
    loop_->assertInLoopThread();

    struct sockaddr_in peer{};
    socklen_t addrlen = sizeof(peer);
    if (::getpeername(sockfd,
                       reinterpret_cast<struct sockaddr*>(&peer),
                       &addrlen) < 0) {
        LOG_ERROR("TcpClient::newConnection getpeername failed: %s",
                  strerror(errno));
    }
    struct sockaddr_in local{};
    addrlen = sizeof(local);
    ::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&local),
                  &addrlen);

    char buf[32];
    ::snprintf(buf, sizeof(buf), ":%d", ntohs(peer.sin_port));
    std::string connName = name_ + "#" + std::to_string(nextConnId_++) + buf;

    auto conn = std::make_shared<TcpConnection>(
        loop_, connName, sockfd, local, peer);
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(
        [this](const TcpConnectionPtr& c) { this->removeConnection(c); });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }

    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_.reset();
    }
    loop_->queueInLoop([conn] { conn->connectDestroyed(); });

    if (retry_ && connect_) {
        // 指数退避重连（原实现立即重连：对端接受后马上关闭时形成无间隔的连接风暴）。
        // 连接存活超过 10 秒视为正常，退避重置。
        double lived = Timestamp::now() - connectedAt_;
        if (lived > 10.0) reconnectDelay_ = 0.5;
        double delay = reconnectDelay_;
        reconnectDelay_ = std::min(reconnectDelay_ * 2, 30.0);
        LOG_INFO("TcpClient [%s] reconnecting in %.1fs", name_.c_str(), delay);
        std::weak_ptr<Connector> wc = connector_;
        auto allowed = reconnectAllowed_;
        loop_->runAfter(delay, [wc, allowed] {
            auto c = wc.lock();
            if (c && allowed->load()) c->restart();
        });
    }
}

}  // namespace streaming
