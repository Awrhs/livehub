#include "api/HttpServer.hpp"
#include "api/Json.hpp"
#include "core/Logger.hpp"

#include <algorithm>
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
            if (req.hasError()) {
                // 非法请求：回错误码后关闭连接（流已不可信，不再继续解析）
                HttpResponse resp(true);
                int st = req.errorStatus();
                resp.setStatusCode(static_cast<HttpStatusCode>(st));
                resp.setStatusMessage(st == 413   ? "Payload Too Large"
                                      : st == 431 ? "Request Header Fields Too Large"
                                      : st == 501 ? "Not Implemented"
                                                  : "Bad Request");
                resp.setCloseConnection(true);
                Buffer out;
                resp.appendToBuffer(&out);
                conn->send(out.retrieveAllAsString());
                conn->shutdown();
                buf->retrieveAll();
            }
            // 否则数据不完整，等待更多数据
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
            // 注意：必须序列化到局部 Buffer。若传入 conn->outputBuffer()，
            // sendInLoop 会把数据追加回自身再被 retrieveAll 清空，响应丢失。
            Buffer out;
            resp.appendToBuffer(&out);
            conn->send(&out);
            finishLog();
            if (resp.closeConnection()) conn->shutdown();
            return;
        }
    }

    // 默认 JSON 头
    bool routed = router_.dispatch(req, resp);

    // 延迟响应：结果由处理器稍后发送
    if (routed && resp.isDeferred()) {
        finishLog();
        resp.deferredCallback()(conn);
        return;
    }

    sendResponse(conn, resp, routed);
    finishLog();
}

void HttpServer::sendResponse(const std::shared_ptr<TcpConnection>& conn, HttpResponse& resp,
                              bool routed) {
    // 序列化头部到局部 Buffer（见上方注释，不能用 conn->outputBuffer()）
    Buffer out;
    resp.appendToBuffer(&out);

    // 文件 body：pread 读入后随头部一起交给连接的输出缓冲。
    // 原实现在 Reactor 线程里对非阻塞 socket 做 sendfile 忙等（EAGAIN 自旋），
    // 慢客户端会卡死整个事件循环，且可能与排队中的头部乱序。
    // TODO(perf): 在 TcpConnection 中实现带 EPOLLOUT 续传的 sendfile 零拷贝。
    if (resp.hasFileBody()) {
        int fd = resp.fileFd();
        if (routed) {
            size_t toSend = resp.isRange()
                                ? (resp.rangeEnd() - resp.rangeStart() + 1)
                                : resp.fileSize();
            off_t offset = resp.isRange()
                               ? static_cast<off_t>(resp.rangeStart())
                               : 0;
            char chunk[64 * 1024];
            size_t sent = 0;
            while (sent < toSend) {
                size_t want = std::min(sizeof(chunk), toSend - sent);
                ssize_t n = ::pread(fd, chunk, want, offset);
                if (n <= 0) break;
                out.append(chunk, static_cast<size_t>(n));
                offset += n;
                sent += static_cast<size_t>(n);
            }
        }
        ::close(fd);
    }
    conn->send(&out);

    // 流式响应：把连接交给处理器持续推送（HTTP-FLV 等）
    if (routed && resp.isStreaming() && resp.streamCallback()) {
        resp.streamCallback()(conn);
        return;
    }

    // 短连接：发完关闭
    if (resp.closeConnection()) {
        conn->shutdown();
    }
}

}  // namespace streaming
