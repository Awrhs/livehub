#include "net/TcpServer.hpp"
#include "net/EventLoop.hpp"
#include "net/EventLoopThreadPool.hpp"
#include "core/Logger.hpp"

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace streaming {

TcpServer::TcpServer(EventLoop* loop, uint16_t port,
                     const std::string& ip, Option option)
    : loop_(loop),
      ipPort_(ip + ":" + std::to_string(port)),
      name_("server-" + std::to_string(port)),
      acceptor_(new Acceptor(loop, port, ip)),
      threadPool_(new EventLoopThreadPool(loop, name_)),
      started_(0),
      nextConnId_(1),
      idleTimeout_(0) {
    acceptor_->setNewConnectionCallback(
        [this](int fd, const struct sockaddr_in& addr) {
            this->newConnection(fd, addr);
        });
    (void)option;
}

TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    LOG_INFO("TcpServer [%s] destruct", name_.c_str());
    for (auto& item : connections_) {
        std::shared_ptr<TcpConnection> conn = item.second;
        item.second.reset();
        conn->getLoop()->runInLoop(
            [conn] { conn->connectDestroyed(); });
    }
}

void TcpServer::start() {
    if (started_.exchange(1) == 0) {
        // 先启动 subReactor 线程池
        threadPool_->start(threadInitCallback_);
        // baseLoop 负责 accept
        loop_->runInLoop([this] { acceptor_->listen(); });
        LOG_INFO("TcpServer [%s] listening on %s with %d IO threads",
                 name_.c_str(), ipPort_.c_str(),
                 threadPool_->getAllLoops().size() - 1);
    }
}

void TcpServer::newConnection(int sockfd,
                              const struct sockaddr_in& peerAddr) {
    loop_->assertInLoopThread();

    // 轮询选一个 subLoop（无 pool 时返回 baseLoop）
    EventLoop* ioLoop = threadPool_->getNextLoop();

    char buf[32];
    ::snprintf(buf, sizeof(buf), ":%d", ntohs(peerAddr.sin_port));
    std::string connName = name_ + "#" + std::to_string(nextConnId_++) + buf;

    struct sockaddr_in localAddr{};
    socklen_t addrlen = sizeof(localAddr);
    ::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&localAddr),
                  &addrlen);

    std::shared_ptr<TcpConnection> conn(
        new TcpConnection(ioLoop, connName, sockfd, localAddr, peerAddr));
    connections_[connName] = conn;
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(
        [this](const std::shared_ptr<TcpConnection>& c) {
            this->removeConnection(c);
        });

    // 空闲超时：在 ioLoop 中定时检查
    if (idleTimeout_ > 0) {
        std::weak_ptr<TcpConnection> weakConn = conn;
        ioLoop->runEvery(1.0, [weakConn, this] {
            std::shared_ptr<TcpConnection> c = weakConn.lock();
            if (!c || !c->connected()) return;
            double idle = Timestamp::now() - c->lastReceiveTime();
            if (idle > idleTimeout_) {
                LOG_WARN("connection [%s] idle %.1fs, force close",
                         c->name().c_str(), idle);
                c->forceClose();
            }
        });
    }

    LOG_INFO("new connection [%s] from %s", connName.c_str(),
             TcpConnection::addrToString(peerAddr).c_str());

    // connectEstablished 必须在连接所属 ioLoop 中执行
    ioLoop->runInLoop([conn] { conn->connectEstablished(); });
}

void TcpServer::removeConnection(const std::shared_ptr<TcpConnection>& conn) {
    // 可能从连接所属 ioLoop 调用，切回 baseLoop 修改 map
    loop_->runInLoop(
        [this, conn] { this->removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(
    const std::shared_ptr<TcpConnection>& conn) {
    loop_->assertInLoopThread();
    LOG_INFO("remove connection [%s]", conn->name().c_str());
    size_t n = connections_.erase(conn->name());
    (void)n;
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn] { conn->connectDestroyed(); });
}

}  // namespace streaming
