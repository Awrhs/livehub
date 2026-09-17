#pragma once

#include "core/NonCopyable.hpp"
#include "net/Channel.hpp"
#include "net/Timer.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>

namespace streaming {

class EventLoop;

/**
 * Connector: 非阻塞 TCP 连接器。
 *
 * 设计要点：
 * - 非阻塞 connect，监听 writable/error，getsockopt(SO_ERROR) 判断结果
 * - 失败时指数退避重试（0.5s → 1s → 2s ...，上限 30s），用 TimerQueue
 * - 连接成功后通过 NewConnectionCallback 把 fd 交给上层（TcpClient）
 *
 * 简历亮点：非阻塞 connect、重试退避、自管理 fd 生命周期
 */
class Connector : NonCopyable,
                  public std::enable_shared_from_this<Connector> {
public:
    using NewConnectionCallback = std::function<void(int sockfd)>;

    enum State { kDisconnected, kConnecting, kConnected };

    Connector(EventLoop* loop, const std::string& ip, uint16_t port);
    ~Connector();

    void setNewConnectionCallback(NewConnectionCallback cb) {
        newConnectionCallback_ = std::move(cb);
    }

    void start();       // 可跨线程
    void restart();     // 必须在 loop 线程
    void stop();        // 可跨线程

    State state() const { return state_; }

private:
    void startInLoop();
    void stopInLoop();
    void connect();
    void connecting(int sockfd);
    void handleWrite();
    void handleError();
    void retry(int sockfd);

    int removeAndResetChannel();
    void resetChannel();
    void setState(State s) { state_ = s; }

    static const int kMaxRetryDelayMs;
    static const int kInitRetryDelayMs;

    EventLoop* loop_;
    std::string ip_;
    uint16_t port_;
    struct sockaddr_in serverAddr_;
    std::atomic<State> state_;
    bool connect_;
    std::unique_ptr<Channel> channel_;
    NewConnectionCallback newConnectionCallback_;
    int retryDelayMs_;
    TimerId retryTimerId_;
};

}  // namespace streaming
