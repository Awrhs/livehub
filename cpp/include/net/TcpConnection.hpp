#pragma once

#include "core/NonCopyable.hpp"
#include "core/Buffer.hpp"
#include "net/Channel.hpp"
#include "net/Socket.hpp"

#include <any>
#include <arpa/inet.h>
#include <functional>
#include <memory>
#include <string>

namespace streaming {

class EventLoop;
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

/**
 * TcpConnection: 一条已建立的 TCP 连接。
 *
 * 设计要点：
 * - 持有 Socket（fd 所有权）+ Channel + input/output Buffer
 * - enable_shared_from_this：保证异步回调时对象存活
 * - 提供应用层回调：onConnection / onMessage / onWriteComplete / onClose
 * - send/shutdown 线程安全（通过 runInLoop 切换到 loop 线程）
 *
 * 简历亮点：连接生命周期管理、shared_from_this、跨线程安全发送
 */
class TcpConnection
    : NonCopyable,
      public std::enable_shared_from_this<TcpConnection> {
public:
    using ConnectionCallback =
        std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback =
        std::function<void(const std::shared_ptr<TcpConnection>&,
                           Buffer*, Timestamp)>;
    using WriteCompleteCallback =
        std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using HighWaterMarkCallback =
        std::function<void(const std::shared_ptr<TcpConnection>&, size_t)>;
    using CloseCallback =
        std::function<void(const std::shared_ptr<TcpConnection>&)>;

    /// TCP_INFO 统计（getsockopt TCP_INFO），供 metrics 使用
    struct TcpInfo {
        int state = 0;
        uint32_t rtt = 0;        // 微秒
        uint32_t lost = 0;       // 丢包
        uint32_t retrans = 0;    // 重传
    };

    TcpConnection(EventLoop* loop, const std::string& name, int sockfd,
                  const struct sockaddr_in& localAddr,
                  const struct sockaddr_in& peerAddr);
    ~TcpConnection();

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    int fd() const { return socket_.fd(); }
    bool connected() const { return state_ == kConnected; }
    bool disconnected() const { return state_ == kDisconnected; }

    void send(const std::string& message);
    void send(Buffer* message);
    void shutdown();       // 关闭写端
    void forceClose();     // 强制关闭

    void setConnectionCallback(ConnectionCallback cb) {
        connectionCallback_ = std::move(cb);
    }
    void setMessageCallback(MessageCallback cb) {
        messageCallback_ = std::move(cb);
    }
    void setWriteCompleteCallback(WriteCompleteCallback cb) {
        writeCompleteCallback_ = std::move(cb);
    }
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, size_t highWaterMark) {
        highWaterMarkCallback_ = std::move(cb);
        highWaterMark_ = highWaterMark;
    }
    void setCloseCallback(CloseCallback cb) {
        closeCallback_ = std::move(cb);
    }

    /// 内部使用：连接建立后调用
    void connectEstablished();
    /// 内部使用：连接销毁前调用
    void connectDestroyed();

    Buffer* inputBuffer() { return &inputBuffer_; }
    Buffer* outputBuffer() { return &outputBuffer_; }

    /// 每收到一次数据刷新一次（用于空闲超时统计）
    Timestamp lastReceiveTime() const { return lastReceiveTime_; }

    /// 获取 TCP_INFO（RTT/丢包/重传），失败返回 false
    bool tcpInfo(TcpInfo* info) const;

    /// 用户上下文（应用层可绑定状态）
    void setContext(const std::any& context) { context_ = context; }
    const std::any& getContext() const { return context_; }

    static std::string addrToString(const struct sockaddr_in& addr) {
        char buf[32];
        ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        uint16_t port = ::ntohs(addr.sin_port);
        return std::string(buf) + ":" + std::to_string(port);
    }

    /// 对端地址（IP 字符串，不含端口）
    std::string peerIp() const {
        char buf[32];
        ::inet_ntop(AF_INET, &peerAddr_.sin_addr, buf, sizeof(buf));
        return std::string(buf);
    }
    const struct sockaddr_in& peerAddr() const { return peerAddr_; }

private:
    enum State { kDisconnected, kConnecting, kConnected, kDisconnecting };

    void handleRead(Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop(const void* data, size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();

    void setState(State s) { state_ = s; }

    EventLoop* loop_;
    const std::string name_;
    State state_;
    Socket socket_;
    Channel channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;

    struct sockaddr_in localAddr_;
    struct sockaddr_in peerAddr_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    CloseCallback closeCallback_;

    size_t highWaterMark_ = 64 * 1024 * 1024;  // 64MB 默认高水位
    Timestamp lastReceiveTime_;

    std::any context_;
};

}  // namespace streaming
