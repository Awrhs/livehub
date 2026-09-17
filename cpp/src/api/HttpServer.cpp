#include "api/HttpServer.hpp"
#include "api/Json.hpp"
#include "core/Logger.hpp"

#include <cctype>

#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

namespace streaming {

struct HttpContext {
    HttpRequest req;
};

void HttpServer::onConnection(const std::shared_ptr<TcpConnection>& conn) {
    if (conn->connected()) {
        conn->setContext(HttpContext{});
    }
}

void HttpServer::onMessage(const std::shared_ptr<TcpConnection>& conn,
                           Buffer* buf) {
    auto& ctx = std::any_cast<HttpContext&>(const_cast<std::any&>(
        conn->getContext()));
    HttpRequest& req = ctx.req;

    // 循环解析（支持 pipeline 与长连接多次请求）
    while (buf->readableBytes() > 0) {
        bool ok = req.parse(buf);
        if (!ok) {
            // 数据不完整，等待更多数据
            return;
        }
        if (req.done()) {
            handleRequest(conn, req);
            req.reset();
        }
    }
}

void HttpServer::handleRequest(const std::shared_ptr<TcpConnection>& conn,
                               HttpRequest& req) {
    // WebSocket / 协议升级（在中间件与路由之前处理）
    if (upgradeCb_) {
        std::string upgrade = req.header("Upgrade");
        std::string lower;
        lower.reserve(upgrade.size());
        for (char c : upgrade) lower.push_back(std::tolower(c));
        if (!lower.empty() && lower.find("websocket") != std::string::npos) {
            if (upgradeCb_(conn, req)) {
                return;  // 已接管连接
            }
        }
    }

    req.clientIp = conn->peerIp();
    Timestamp start = Timestamp::now();
    HttpResponse resp(true);  // 简化：默认关闭连接

    auto finishLog = [&]() {
        if (afterRequest_) {
            double sec = Timestamp::now() - start;
            afterRequest_(req, resp, sec);
        }
    };

    // 中间件链：任一返回 false 表示已处理/拦截，直接发送响应
    for (const auto& mw : middlewares_) {
        if (!mw(conn, req, resp)) {
            Buffer* out = conn->outputBuffer();
            resp.appendToBuffer(out);
            conn->send(out);
            finishLog();
            if (resp.closeConnection()) conn->shutdown();
            return;
        }
    }

    // 默认 JSON 头
    bool routed = router_.dispatch(req, resp);

    // 序列化头部到 output buffer
    Buffer* out = conn->outputBuffer();
    resp.appendToBuffer(out);

    // 先把头部发出去
    conn->send(out);

    // 若有文件 body，用 sendfile 零拷贝
    if (routed && resp.hasFileBody()) {
        int fd = resp.fileFd();
        size_t toSend = resp.isRange()
                            ? (resp.rangeEnd() - resp.rangeStart() + 1)
                            : resp.fileSize();
        off_t offset = resp.isRange()
                           ? static_cast<off_t>(resp.rangeStart())
                           : 0;
        size_t sent = 0;
        while (sent < toSend) {
            ssize_t n = ::sendfile(conn->fd(), fd, &offset, toSend - sent);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                }
                break;
            }
            sent += static_cast<size_t>(n);
        }
        ::close(fd);
    }

    // 流式响应：把连接交给处理器持续推送（HTTP-FLV 等）
    if (routed && resp.isStreaming() && resp.streamCallback()) {
        finishLog();
        resp.streamCallback()(conn);
        return;
    }

    finishLog();

    // 短连接：发完关闭
    if (resp.closeConnection()) {
        conn->shutdown();
    }
}

}  // namespace streaming
