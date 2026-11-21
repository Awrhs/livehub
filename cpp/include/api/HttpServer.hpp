#pragma once

#include "core/NonCopyable.hpp"
#include "net/EventLoop.hpp"
#include "net/TcpServer.hpp"
#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/Router.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace streaming {

/**
 * Middleware: 返回 false 表示拦截（已写响应），true 继续下一个中间件/路由。
 */
using Middleware =
    std::function<bool(const std::shared_ptr<TcpConnection>&, HttpRequest&,
                       HttpResponse&)>;

/**
 * HttpServer: 基于 TcpServer 的 HTTP/1.1 服务器。
 *
 * 设计要点：
 * - 每条连接绑定一个 HttpRequest 解析器（通过 context）
 * - 收到数据后 parse，解析完成后 dispatch 到 Router
 * - 字符串 body 直接 append 到 output buffer
 * - 文件 body：先发头部，再用 sendfile 零拷贝发送
 *
 * 简历亮点：HTTP 状态机、sendfile 零拷贝、长连接复用
 */
class HttpServer : NonCopyable {
public:
    HttpServer(EventLoop* loop, uint16_t port,
               const std::string& ip = "0.0.0.0")
        : server_(loop, port, ip) {
        server_.setMessageCallback(
            [this](const std::shared_ptr<TcpConnection>& conn,
                   Buffer* buf, Timestamp) {
                this->onMessage(conn, buf);
            });
        server_.setConnectionCallback(
            [this](const std::shared_ptr<TcpConnection>& conn) {
                this->onConnection(conn);
            });
    }

    Router& router() { return router_; }

    /// 路由便捷方法
    void get(const std::string& path, Handler h) { router_.get(path, std::move(h)); }
    void post(const std::string& path, Handler h) {
        router_.post(path, std::move(h));
    }
    template <class... Args>
    void add(Args&&... args) {
        router_.add(std::forward<Args>(args)...);
    }

    /// 添加中间件（按添加顺序执行）
    void use(Middleware mw) { middlewares_.push_back(std::move(mw)); }

    /// 请求处理完成后的回调（用于访问日志，可观测 status/bytes/耗时）
    using AfterRequest =
        std::function<void(const HttpRequest&, const HttpResponse&,
                           double elapsedSeconds)>;
    void setAfterRequest(AfterRequest cb) { afterRequest_ = std::move(cb); }

    void start() { server_.start(); }

    void setThreadNum(int n) { server_.setThreadNum(n); }
    void setIdleTimeout(int seconds) { server_.setIdleTimeout(seconds); }

    /// WebSocket / 协议升级回调：返回 true 表示已接管连接（已写 101 并
    /// 通过 conn->setMessageCallback 安装了新的数据处理器），HTTP 层
    /// 不再解析后续数据。用于 WebSocket-FLV。
    using UpgradeCallback =
        std::function<bool(const std::shared_ptr<TcpConnection>&,
                           HttpRequest&)>;
    void setUpgradeCallback(UpgradeCallback cb) { upgradeCb_ = std::move(cb); }

private:
    void onConnection(const std::shared_ptr<TcpConnection>& conn);
    void onMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);
    void handleRequest(const std::shared_ptr<TcpConnection>& conn,
                       HttpRequest& req);

    TcpServer server_;
    Router router_;
    std::vector<Middleware> middlewares_;
    AfterRequest afterRequest_;
    UpgradeCallback upgradeCb_;
};

}  // namespace streaming
