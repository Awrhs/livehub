#pragma once

#include "core/NonCopyable.hpp"
#include "net/TcpClient.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <functional>

namespace streaming {

class HttpServer;
class EventLoop;
class TcpConnection;

/**
 * ProxyHandler: HTTP 反向代理 + 重定向调度。
 *
 * 两种模式：
 * 1) 代理转发：本节点作为反向代理，按 LoadBalancer 选一个上游，
 *    建立出站连接，把请求行/头/body 原样转发，并把上游响应流式回传。
 * 2) 302 重定向：按一致性哈希选节点，返回 Location 让客户端直连边缘。
 *
 * 这里实现一个可工作的最小代理：转发请求并桥接双向 body，支持 chunked
 * 透传（不在本层解析，直接字节转发）。
 */
class ProxyHandler : NonCopyable {
public:
    using NodeResolver =
        std::function<std::string(/*out*/ std::string& host,
                                   /*out*/ uint16_t& port)>;

    explicit ProxyHandler(EventLoop* loop);

    /// 注册代理路由 GET /proxy/* （或在 main 中通过 registerRoutes 挂载）
    void registerRoutes(HttpServer& server);

    /// 设置上游选择器：给定请求 path/key，返回上游 host:port
    void setUpstreamSelector(
        std::function<std::pair<std::string, uint16_t>(
            const std::string& key)>
            sel) {
        selector_ = std::move(sel);
    }

    /// 302 重定向模式：返回 true 表示已用 Location 响应
    void setRedirectMode(bool on) { redirectMode_ = on; }
    bool redirectMode() const { return redirectMode_; }

private:
    struct Bridge;

    EventLoop* loop_;
    std::function<std::pair<std::string, uint16_t>(const std::string&)>
        selector_;
    bool redirectMode_ = false;
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Bridge>> bridges_;
};

}  // namespace streaming
