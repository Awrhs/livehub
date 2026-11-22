#pragma once

#include "core/Timestamp.hpp"

#include "core/NonCopyable.hpp"
#include "net/Connector.hpp"
#include "net/TcpConnection.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace streaming {

class EventLoop;

/**
 * TcpClient: 非阻塞 TCP 客户端，支持断线重连。
 *
 * 设计要点：
 * - 通过 Connector 发起非阻塞 connect
 * - 连接成功后构造 TcpConnection 并派发回调
 * - enableRetry() 打开断线自动重连（指数退避）
 *
 * 供集群回源、RTMP pull、健康检查使用。
 */
class TcpClient : NonCopyable {
public:
    TcpClient(EventLoop* loop, const std::string& ip, uint16_t port,
              const std::string& name = "client");
    ~TcpClient();

    void setConnectionCallback(TcpConnection::ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }
    void setMessageCallback(TcpConnection::MessageCallback cb) {
        messageCallback_ = std::move(cb);
    }
    void setWriteCompleteCallback(TcpConnection::WriteCompleteCallback cb) {
        writeCompleteCallback_ = std::move(cb);
    }

    /// 打开/关闭断线重连，必须在 connect 前调用
    void enableRetry(bool on = true) { retry_ = on; }

    void connect();
    void disconnect();
    void stop();

    TcpConnectionPtr connection() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connection_;
    }

    EventLoop* getLoop() const { return loop_; }
    bool retry() const { return retry_; }
    const std::string& name() const { return name_; }

private:
    void newConnection(int sockfd);
    void removeConnection(const TcpConnectionPtr& conn);

    EventLoop* loop_;
    std::shared_ptr<Connector> connector_;
    const std::string name_;

    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;  // GUARDED_BY(mutex_)

    TcpConnection::ConnectionCallback connectionCallback_;
    TcpConnection::MessageCallback messageCallback_;
    TcpConnection::WriteCompleteCallback writeCompleteCallback_;

    std::atomic<bool> retry_;
    Timestamp connectedAt_;
    // 延迟重连任务的存活标志：客户端析构/断开后置 false，避免定时器回调访问已销毁对象或复活连接
    std::shared_ptr<std::atomic<bool>> reconnectAllowed_ =
        std::make_shared<std::atomic<bool>>(true);
    double reconnectDelay_ = 0.5;  // 秒，指数退避
    std::atomic<bool> connect_;
    int nextConnId_;
};

}  // namespace streaming
