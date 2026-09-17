#include "rtsp/RtspServer.hpp"
#include "core/Logger.hpp"
#include "rtmp/MediaStream.hpp"
#include "rtsp/RtspSession.hpp"

#include <any>
#include <memory>

namespace streaming {

RtspServer::RtspServer(EventLoop* loop, uint16_t port)
    : loop_(loop), port_(port), server_(loop, port, "0.0.0.0") {
    server_.setConnectionCallback(
        [this](const TcpConnectionPtr& c) { onConnection(c); });
    server_.setMessageCallback(
        [this](const TcpConnectionPtr& c, Buffer* b, Timestamp t) {
            onMessage(c, b, t);
        });
}

void RtspServer::start() {
    server_.start();
    LOG_INFO("RtspServer listening on TCP %d", port_);
}

void RtspServer::onConnection(const TcpConnectionPtr& conn) {
    if (!conn->connected()) {
        // 关闭时取出 session 并释放
        auto& ctx = const_cast<std::any&>(conn->getContext());
        if (ctx.has_value()) {
            auto sess = std::any_cast<std::shared_ptr<RtspSession>>(&ctx);
            if (sess) (*sess)->close();
        }
        return;
    }
}

void RtspServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf,
                           Timestamp) {
    // 首次数据前从 DESCRIBE/SETUP 的 URL 拿 stream key 略复杂，
    // 这里简化：在连接的 context 中延迟创建 session（首个请求解析后）。
    auto& ctx = const_cast<std::any&>(conn->getContext());
    auto* sessPtr =
        ctx.has_value()
            ? std::any_cast<std::shared_ptr<RtspSession>>(&ctx)
            : nullptr;

    // 先窥视 URL 解析 stream key（rtsp://host/live/STREAM[/trackID]）
    // 为支持首请求创建 session，先尝试解析当前 buffer 中的请求行。
    std::string key;
    if (!sessPtr) {
        std::string peek(buf->peek(), buf->readableBytes());
        auto sp = peek.find(' ');
        if (sp != std::string::npos) {
            auto sp2 = peek.find(' ', sp + 1);
            std::string url = peek.substr(sp + 1, sp2 - sp - 1);
            auto p = url.find("/live/");
            if (p != std::string::npos) {
                key = url.substr(p + 6);
                auto slash = key.find('/');
                if (slash != std::string::npos) key = key.substr(0, slash);
            }
        }
        if (key.empty()) {
            // 等更多数据
            return;
        }
        auto stream = resolver_ ? resolver_(key) : nullptr;
        if (!stream) {
            // 流不存在，返回 404 文本后断开
            conn->send("RTSP/1.0 404 Stream Not Found\r\n\r\n");
            conn->shutdown();
            return;
        }
        auto sess = std::make_shared<RtspSession>(conn, stream);
        ctx = sess;
        sessPtr = &std::any_cast<std::shared_ptr<RtspSession>&>(ctx);
    }

    if (sessPtr && *sessPtr) {
        (*sessPtr)->onRead(buf);
    }
}

}  // namespace streaming
