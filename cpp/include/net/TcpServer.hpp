#pragma once

#include "core/NonCopyable.hpp"
#include "net/Acceptor.hpp"
#include "net/EventLoop.hpp"
#include "net/EventLoopThreadPool.hpp"
#include "net/TcpConnection.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

namespace streaming {

/**
 * TcpServer: TCP 服务器，管理连接的生命周期。
 *
 * 设计要点：
 * - mainReactor（baseLoop）负责 accept，subReactor 线程池处理已建立连接
 * - one loop per thread：setThreadNum(N) 启用 N 个 subLoop
 * - 用 shared_ptr 持有 TcpConnection，连接关闭时从所属 loop 移除
 * - 提供应用层回调注册接口
 *
 * 简历亮点：mainReactor+subReactor 多线程模型、shared_ptr 跨 loop 生命周期
 */
class TcpServer : NonCopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    enum Option { kNoReusePort, kReusePort };

    TcpServer(EventLoop* loop, uint16_t port,
              const std::string& ip = "0.0.0.0",
              Option option = kReusePort);
    ~TcpServer();

    /// 设置应用层回调
    void setConnectionCallback(TcpConnection::ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }
    void setMessageCallback(TcpConnection::MessageCallback cb) {
        messageCallback_ = std::move(cb);
    }
    void setWriteCompleteCallback(TcpConnection::WriteCompleteCallback cb) {
        writeCompleteCallback_ = std::move(cb);
    }
    void setThreadInitCallback(ThreadInitCallback cb) {
        threadInitCallback_ = std::move(cb);
    }

    /// 设置 subReactor 线程数，必须在 start() 前调用；0=单线程
    void setThreadNum(int numThreads) {
        threadPool_->setThreadNum(numThreads);
    }

    /// 设置空闲连接超时（秒），0 表示不踢除
    void setIdleTimeout(int seconds) { idleTimeout_ = seconds; }
    /// 连接数上限（0 = 不限）：超限的新连接被立即关闭
    void setConnectionLimits(int total, int perIp) {
        maxConnections_ = total;
        maxPerIp_ = perIp;
    }

    void start();
    /// 有序关闭（须在 baseLoop 线程、baseLoop 已停止后调用）：
    /// 强制关闭全部连接，退出并 join 所有 IO 线程。之后不再有任何连接回调。
    void stop();

    EventLoop* getLoop() const { return loop_; }
    EventLoopThreadPool* threadPool() const { return threadPool_.get(); }

private:
    void newConnection(int sockfd, const struct sockaddr_in& peerAddr);
    void removeConnection(const std::shared_ptr<TcpConnection>& conn);
    void removeConnectionInLoop(const std::shared_ptr<TcpConnection>& conn);

    using ConnectionMap =
        std::unordered_map<std::string, std::shared_ptr<TcpConnection>>;

    EventLoop* loop_;  // 主 loop（accept 用）
    const std::string ipPort_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    std::atomic<bool> stopped_{false};
    std::atomic<int> started_;
    int nextConnId_;
    int idleTimeout_ = 0;  // 秒，0=不超时
    ConnectionMap connections_;
    int maxConnections_ = 0;
    int maxPerIp_ = 0;
    std::unordered_map<uint32_t, int> perIp_;            // baseLoop 线程访问
    std::unordered_map<std::string, uint32_t> connIp_;   // 连接名 → IP
    uint64_t rejected_ = 0;

    TcpConnection::ConnectionCallback connectionCallback_;
    TcpConnection::MessageCallback messageCallback_;
    TcpConnection::WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_;
};

}  // namespace streaming
