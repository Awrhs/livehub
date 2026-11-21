#include "net/TcpServer.hpp"
#include <vector>
#include <map>
#include <future>
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

void TcpServer::stop() {
    if (stopped_.exchange(true)) return;
    // 阶段一：在每个连接所属的 IO 线程内关闭并销毁连接（IO 线程的 EventLoop 是线程栈对象，
    // 线程退出后即失效，不能在线程外操作其 Channel），并等待全部完成。
    // 完成后所有连接均为断开态，send() 不再向任何 loop 投递任务。
    std::map<EventLoop*, std::vector<std::shared_ptr<TcpConnection>>> byLoop;
    for (auto& item : connections_) {
        if (item.second) byLoop[item.second->getLoop()].push_back(item.second);
    }
    std::vector<std::future<void>> done;
    for (auto& entry : byLoop) {
        EventLoop* loop = entry.first;
        const auto conns = entry.second;
        if (loop->isInLoopThread()) {
            // 连接就在当前（已停止的 base）loop 上：直接执行，避免等待自身造成死锁
            for (const auto& c : conns) c->shutdownInOwnLoop();
            continue;
        }
        auto p = std::make_shared<std::promise<void>>();
        done.push_back(p->get_future());
        loop->queueInLoop([conns, p] {
            for (const auto& c : conns) c->shutdownInOwnLoop();
            p->set_value();
        });
    }
    for (auto& f : done) f.wait();
    connections_.clear();
    // 阶段二：退出并 join 所有 IO 线程
    threadPool_->stop();
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

    // 连接数上限（防止单个来源或洪泛耗尽 fd/内存）
    uint32_t ip = peerAddr.sin_addr.s_addr;
    if ((maxConnections_ > 0 && static_cast<int>(connections_.size()) >= maxConnections_) ||
        (maxPerIp_ > 0 && perIp_[ip] >= maxPerIp_)) {
        if ((rejected_++ & 1023) == 0)  // 首次及每 1024 次告警一次
            LOG_WARN("TcpServer [%s] connection limit reached (total=%zu, ip=%s), rejected %llu",
                     name_.c_str(), connections_.size(),
                     TcpConnection::addrToString(peerAddr).c_str(),
                     static_cast<unsigned long long>(rejected_));
        ::close(sockfd);
        return;
    }

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
    ++perIp_[ip];
    connIp_[connName] = ip;
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(
        [this](const std::shared_ptr<TcpConnection>& c) {
            this->removeConnection(c);
        });

    // 空闲超时由连接自身管理（读写均无进展才断开）
    if (idleTimeout_ > 0) conn->setIdleTimeout(idleTimeout_);

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
    auto ipIt = connIp_.find(conn->name());
    if (ipIt != connIp_.end()) {
        auto pi = perIp_.find(ipIt->second);
        if (pi != perIp_.end() && --pi->second <= 0) perIp_.erase(pi);
        connIp_.erase(ipIt);
    }
    (void)n;
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn] { conn->connectDestroyed(); });
}

}  // namespace streaming
