#include "protocol/HttpFlvHandler.hpp"

#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "api/HttpRequest.hpp"
#include "core/Buffer.hpp"
#include "core/Logger.hpp"
#include "net/TcpConnection.hpp"
#include "protocol/FlvMuxer.hpp"
#include "rtmp/MediaStream.hpp"

#include <cstring>
#include <memory>

namespace streaming {

namespace {

// 一次 FLV 订阅会话：自持有，断开时自行注销。
struct FlvSession : public std::enable_shared_from_this<FlvSession> {
    std::weak_ptr<TcpConnection> conn;
    std::shared_ptr<MediaStream> stream;
    MediaStream::SubscriberId subId = 0;

    static std::shared_ptr<FlvSession> start(
        const std::shared_ptr<TcpConnection>& c,
        const std::shared_ptr<MediaStream>& s) {
        auto sess = std::shared_ptr<FlvSession>(new FlvSession());
        sess->conn = c;
        sess->stream = s;

        // 1. 发送 FLV header
        Buffer hdr;
        FlvMuxer::writeHeader(hdr,
                              !s->audioSequenceHeader().empty(),
                              !s->videoSequenceHeader().empty());
        // 2. onMetaData script tag
        auto meta = s->metadata();
        if (!meta.empty()) {
            FlvMuxer::writeScriptTag(hdr, meta);
        }
        c->send(&hdr);

        // 3. 推 GOP（sequence header + 最近关键帧起）
        Buffer gop;
        s->playGop([&gop](const MediaPacket& pkt) {
            FlvMuxer::writeMediaTag(gop, pkt);
        });
        if (gop.readableBytes() > 0) {
            c->send(&gop);
        }

        // 4. 订阅实时包
        sess->subId = s->subscribe(
            [w = sess->weak_from_this()](const MediaPacket& pkt) {
                auto self = w.lock();
                if (!self) return;
                auto c2 = self->conn.lock();
                if (!c2 || !c2->connected()) {
                    // 连接已断开，注销订阅
                    if (self->stream && self->subId != 0) {
                        self->stream->unsubscribe(self->subId);
                        self->subId = 0;
                    }
                    return;
                }
                Buffer out;
                FlvMuxer::writeMediaTag(out, pkt);
                c2->send(&out);
            });
        return sess;
    }

private:
    FlvSession() = default;
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
