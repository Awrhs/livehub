#include "protocol/WsFlvHandler.hpp"

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "core/Logger.hpp"
#include "net/TcpConnection.hpp"
#include "protocol/FlvMuxer.hpp"
#include "rtmp/FlowControl.hpp"
#include "protocol/WebSocket.hpp"
#include "rtmp/MediaStream.hpp"

#include <cstring>
#include <memory>

namespace streaming {

namespace {

struct WsFlvSession : public std::enable_shared_from_this<WsFlvSession> {
    std::weak_ptr<TcpConnection> conn;
    std::shared_ptr<MediaStream> stream;
    MediaStream::SubscriberId subId = 0;
    FlowControl flow;

    static std::shared_ptr<WsFlvSession> start(
        const std::shared_ptr<TcpConnection>& c,
        const std::shared_ptr<MediaStream>& s) {
        auto sess = std::shared_ptr<WsFlvSession>(new WsFlvSession());
        sess->conn = c;
        sess->stream = s;

        // 先发 FLV header + script + GOP，每个作为一个 binary WebSocket 帧
        auto sendAsBinary = [&c](const Buffer& b) {
            std::vector<uint8_t> frame = WebSocket::buildFrame(
                kWsBinary, reinterpret_cast<const uint8_t*>(b.peek()),
                b.readableBytes());
            c->send(std::string(reinterpret_cast<char*>(frame.data()),
                                frame.size()));
        };

        Buffer hdr;
        FlvMuxer::writeHeader(hdr, !s->audioSequenceHeader().empty(),
                              !s->videoSequenceHeader().empty());
        sendAsBinary(hdr);

        auto meta = s->metadata();
        if (!meta.empty()) {
            Buffer script;
            FlvMuxer::writeScriptTag(script, meta);
            sendAsBinary(script);
        }

        // 原子地回放 GOP 并订阅实时包（每个 tag 一个 binary 帧）
        sess->subId = s->subscribeWithGop(
            [w = sess->weak_from_this()](const MediaPacket& pkt) {
                auto self = w.lock();
                if (!self) return;  // 会话已析构（析构函数会注销订阅）
                auto c2 = self->conn.lock();
                if (!c2 || !c2->connected()) return;
                switch (self->flow.check(pkt, c2->pendingOutputBytes())) {
                    case FlowControl::kDrop:
                        return;
                    case FlowControl::kClose:
                        c2->forceClose();
                        return;
                    case FlowControl::kSend:
                        break;
                }
                Buffer out;
                FlvMuxer::writeMediaTag(out, pkt);
                std::vector<uint8_t> frame = WebSocket::buildFrame(
                    kWsBinary,
                    reinterpret_cast<const uint8_t*>(out.peek()),
                    out.readableBytes());
                c2->send(std::string(reinterpret_cast<char*>(frame.data()),
                                     frame.size()));
            });
        return sess;
    }

    ~WsFlvSession() {
        // 会话随连接释放：从流的订阅列表移除，避免遗留失效回调
        if (stream && subId != 0) stream->unsubscribe(subId);
    }

private:
    WsFlvSession() = default;
};

bool isSafeName(const std::string& s) {
    return !s.empty() && s.find("..") == std::string::npos &&
           s.find('/') == std::string::npos &&
           s.find('\\') == std::string::npos;
}

// 从 path 解析 stream key（/live/xxx.flv 或 /live/xxx）
std::string parseKey(const std::string& path) {
    auto p = path.find("/live/");
    if (p == std::string::npos) return "";
    std::string rest = path.substr(p + 6);
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    if (rest.size() >= 4 && rest.compare(rest.size() - 4, 4, ".flv") == 0)
        rest = rest.substr(0, rest.size() - 4);
    return rest;
}

}  // namespace

WsFlvHandler::WsFlvHandler(StreamResolver resolver)
    : resolver_(std::move(resolver)) {}

void WsFlvHandler::attach(HttpServer& server) {
    server.setUpgradeCallback(
        [this](const std::shared_ptr<TcpConnection>& conn,
               HttpRequest& req) -> bool {
            std::string key = parseKey(req.path());
            if (key.empty() || !isSafeName(key)) return false;

            std::string wsKey = req.header("Sec-WebSocket-Key");
            if (wsKey.empty()) return false;

            auto ms = resolver_ ? resolver_(key) : nullptr;
            if (!ms) {
                conn->send("HTTP/1.1 404 Not Found\r\n\r\n");
                conn->shutdown();
                return true;
            }

            std::string accept = WebSocket::acceptKey(wsKey);
            std::string resp =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " +
                accept +
                "\r\n\r\n";
            conn->send(resp);

            auto sess = WsFlvSession::start(conn, ms);

            // 接管后续数据：解析客户端 WebSocket 帧（ping/pong/close）
            conn->setMessageCallback(
                [sess](const std::shared_ptr<TcpConnection>& c, Buffer* b,
                       Timestamp) {
                    bool ok = WebSocket::parseFrames(
                        *b,
                        [c, sess](WsOpcode op, const uint8_t*, size_t) {
                            // parseFrames 把 ping 透传为 kWsPong 回调
                            if (op == kWsPong) {
                                auto f = WebSocket::buildFrame(kWsPong,
                                                               nullptr, 0);
                                c->send(std::string(
                                    reinterpret_cast<char*>(f.data()),
                                    f.size()));
                            } else if (op == kWsClose) {
                                auto f = WebSocket::buildClose(1000);
                                c->send(std::string(
                                    reinterpret_cast<char*>(f.data()),
                                    f.size()));
                                c->shutdown();
                            }
                            // 客户端文本/二进制忽略（纯下行直播）
                            (void)sess;
                        });
                    if (!ok) c->shutdown();
                });
            return true;
        });
}

}  // namespace streaming
