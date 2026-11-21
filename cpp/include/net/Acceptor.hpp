#pragma once

#include "core/NonCopyable.hpp"
#include "net/Channel.hpp"
#include "net/Socket.hpp"

#include <functional>
#include <memory>

namespace streaming {

class EventLoop;
class InetAddress;

/**
 * Acceptor: 监听 socket，accept 新连接。
 *
 * 设计要点：
 * - 持有 listen socket + 对应 Channel
 * - 有新连接时回调 newConnectionCallback_(connfd, peerAddr)
 * - 防止"惊群"：可设置 SO_REUSEPORT 多进程 accept（后续扩展）
 */
class Acceptor : NonCopyable {
public:
    using NewConnectionCallback = std::function<void(int, const struct sockaddr_in&)>;

    Acceptor(EventLoop* loop, uint16_t port,
             const std::string& ip = "0.0.0.0");
    ~Acceptor();

    void setNewConnectionCallback(NewConnectionCallback cb) {
        newConnectionCallback_ = std::move(cb);
    }

    bool listening() const { return listening_; }
    void listen();

private:
    void handleRead();

    EventLoop* loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
    int idleFd_ = -1;  // 预留 fd，用于 EMFILE 时腾挪
};

}  // namespace streaming
