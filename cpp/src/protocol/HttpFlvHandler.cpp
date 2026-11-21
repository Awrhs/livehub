#include "protocol/HttpFlvHandler.hpp"

#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "api/HttpRequest.hpp"
#include "core/Buffer.hpp"
#include "core/Logger.hpp"
#include "net/TcpConnection.hpp"
#include "protocol/FlvMuxer.hpp"
#include "rtmp/MediaStream.hpp"

#include <atomic>
#include <cstring>
#include <memory>

namespace streaming {

namespace {

// 一次 FLV 订阅会话。
// 生命周期：订阅回调强持有 session；session 只弱引用 stream 与连接，
// 因此流被移除（回调列表释放）或连接断开（下一个包时主动注销）都会释放 session，
// 不会形成 stream→callback→session→stream 的引用环。
// （原实现回调只捕获 weak_ptr 且无任何强引用，session 在 start() 返回后立即析构，
//  导致 HTTP-FLV 只发出响应头、永远没有媒体数据。）
struct FlvSession {
    std::weak_ptr<TcpConnection> conn;
    std::weak_ptr<MediaStream> stream;
    MediaStream::SubscriberId subId = 0;
    std::atomic<bool> done{false};

    static void start(const std::shared_ptr<TcpConnection>& c,
                      const std::shared_ptr<MediaStream>& s) {
        auto sess = std::make_shared<FlvSession>();
        sess->conn = c;
        sess->stream = s;

        // 1. FLV header + onMetaData
        Buffer hdr;
        FlvMuxer::writeHeader(hdr, !s->audioSequenceHeader().empty(),
                              !s->videoSequenceHeader().empty());
        auto meta = s->metadata();
        if (!meta.empty()) FlvMuxer::writeScriptTag(hdr, meta);
        c->send(&hdr);

        // 2. 原子地回放 GOP 并订阅实时包
        sess->subId = s->subscribeWithGop([sess](const MediaPacket& pkt) {
            if (sess->done.load()) return;
            auto c2 = sess->conn.lock();
            if (!c2 || !c2->connected()) {
                sess->finish();
                return;
            }
            Buffer out;
            FlvMuxer::writeMediaTag(out, pkt);
            c2->send(&out);
        });
    }

    void finish() {
        if (done.exchange(true)) return;
        if (auto st = stream.lock()) {
            if (subId != 0) st->unsubscribe(subId);  // 释放回调 → 释放 session
        }
    }
};

bool isSafeName(const std::string& s) {
    return !s.empty() && s.find("..") == std::string::npos &&
           s.find('/') == std::string::npos && s.find('\\') ==
               std::string::npos;
}

}  // namespace

HttpFlvHandler::HttpFlvHandler(StreamResolver resolver)
    : resolver_(std::move(resolver)) {}

void HttpFlvHandler::serve(const std::string& key, HttpResponse& resp,
                           const StreamResolver& resolver) {
    auto ms = resolver ? resolver(key) : nullptr;
    if (!ms) {
        resp.setStatusCode(HttpStatusCode::k404NotFound);
        resp.setContentType("application/json");
        resp.setBody("{\"error\":\"stream not found\"}");
        return;
    }

    resp.setStatusCode(HttpStatusCode::k200Ok);
    resp.setContentType("video/x-flv");
    resp.addHeader("Cache-Control", "no-cache");
    resp.addHeader("Access-Control-Allow-Origin", "*");
    resp.setCloseConnection(false);

    resp.setStreamCallback(
        [ms](const std::shared_ptr<TcpConnection>& conn) {
            // 启动会话；session 由订阅回调自持有，断开时释放
            FlvSession::start(conn, ms);
        });
}

void HttpFlvHandler::registerRoutes(HttpServer& server) {
    // 注意：/live/:stream 单段路由已合并到 HlsHttpHandler，按后缀分发，
    // 避免与 .m3u8 路由因同形 pattern 产生注册顺序冲突。
    server.router().get(
        "/live/:stream",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string file = req.param("stream");
            // 仅处理 .flv
            if (file.size() < 5 ||
                file.compare(file.size() - 4, 4, ".flv") != 0) {
                resp.setStatusCode(HttpStatusCode::k404NotFound);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"not found\"}");
                return;
            }
            std::string key = file.substr(0, file.size() - 4);
            if (!isSafeName(key)) {
                resp.setStatusCode(HttpStatusCode::k400BadRequest);
                resp.setBody("{\"error\":\"bad stream name\"}");
                return;
            }
            serve(key, resp, resolver_);
        });
}

}  // namespace streaming
