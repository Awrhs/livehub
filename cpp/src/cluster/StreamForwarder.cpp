#include "cluster/StreamForwarder.hpp"

#include "core/Buffer.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "net/EventLoop.hpp"
#include "net/TcpClient.hpp"
#include "net/TcpConnection.hpp"
#include "rtmp/Amf0.hpp"
#include "rtmp/Chunk.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <random>

namespace streaming {

namespace {

struct RtmpUrl {
    std::string host;
    uint16_t port = 1935;
    std::string app = "live";
    std::string key;
    bool tls = false;
};

bool parseRtmpUrl(const std::string& url, RtmpUrl& out) {
    std::string rest = url;
    if (rest.rfind("rtmps://", 0) == 0) {
        out.tls = true;
        rest = rest.substr(8);
    } else if (rest.rfind("rtmp://", 0) == 0) {
        rest = rest.substr(7);
    } else {
        // 严格 scheme 白名单：转推只支持 rtmp/rtmps，
        // 其余（http/rtsp/file/... 一律拒绝，避免误配置静默失败。
        return false;
    }
    auto slash = rest.find('/');
    if (slash == std::string::npos) return false;
    std::string hostport = rest.substr(0, slash);
    std::string path = rest.substr(slash + 1);
    auto colon = hostport.find(':');
    if (colon == std::string::npos) {
        out.host = hostport;
        out.port = out.tls ? 443 : 1935;
    } else {
        out.host = hostport.substr(0, colon);
        out.port = static_cast<uint16_t>(std::stoi(hostport.substr(colon + 1)));
    }
    // path = app/key?params 或 app/key
    auto q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    auto s = path.find('/');
    if (s == std::string::npos) {
        out.app = path;
        out.key = "";
    } else {
        out.app = path.substr(0, s);
        out.key = path.substr(s + 1);
    }
    return !out.host.empty() && !out.key.empty();
}

void writeCommand(Buffer& out, const std::string& cmd, double txnId,
                  const std::vector<Amf0Value>& args, uint32_t streamId,
                  uint32_t csid) {
    Buffer payload;
    Amf0Value::str(cmd).encode(payload);
    Amf0Value::number(txnId).encode(payload);
    for (const auto& a : args) a.encode(payload);

    RtmpMessage msg;
    msg.typeId = kMsgCommandAmf0;
    msg.chunkStreamId = csid;
    msg.streamId = streamId;
    msg.timestamp = 0;
    msg.payload.assign(reinterpret_cast<const uint8_t*>(payload.peek()),
                      reinterpret_cast<const uint8_t*>(payload.peek()) +
                          payload.readableBytes());
    ChunkWriter w;
    w.write(out, msg);
}

[[maybe_unused]] void writeData(Buffer& out, const std::vector<Amf0Value>& vals,
               uint32_t streamId, uint32_t csid) {
    Buffer payload;
    for (const auto& v : vals) v.encode(payload);
    RtmpMessage msg;
    msg.typeId = kMsgDataAmf0;
    msg.chunkStreamId = csid;
    msg.streamId = streamId;
    msg.timestamp = 0;
    msg.payload.assign(reinterpret_cast<const uint8_t*>(payload.peek()),
                      reinterpret_cast<const uint8_t*>(payload.peek()) +
                          payload.readableBytes());
    ChunkWriter w;
    w.write(out, msg);
}

}  // namespace

struct StreamForwarder::Target
    : public std::enable_shared_from_this<Target> {
    enum class State {
        kHandshake,
        kConnectSent,
        kCreateSent,
        kPublishSent,
        kPublishing,
        kClosed,
    };

    uint64_t id;
    RtmpUrl url;
    std::string urlStr;
    EventLoop* loop;
    std::unique_ptr<TcpClient> client;
    TcpConnectionPtr conn;
    ChunkParser parser;
    Buffer readBuf;
    State state = State::kHandshake;
    int handshakeBytes = 0;
    std::vector<uint8_t> handshakeC1;
    uint32_t serverChunkSize = 128;
    double nextTxnId = 1.0;
    bool gotCreateResult = false;
    std::atomic<bool> stopped{false};
    int backoffMs = 500;
    std::atomic<uint64_t> reconnects{0};
    std::atomic<uint64_t> bytesSent{0};

    // 队列缓冲（握手/发布前收到的包），最多保留若干，避免无限增长
    static constexpr size_t kMaxQueued = 256;
    std::deque<MediaPacket> queue;

    Target(uint64_t i, const RtmpUrl& u, const std::string& us,
           EventLoop* l)
        : id(i), url(u), urlStr(us), loop(l) {}

    void start() {
        client = std::make_unique<TcpClient>(loop, url.host, url.port,
                                             "fwd-" + std::to_string(id));
        client->enableRetry(false);  // 自己管理重连退避
        auto w = weak_from_this();
        client->setConnectionCallback(
            [w](const TcpConnectionPtr& c) {
                if (auto self = w.lock()) self->onConnect(c);
            });
        client->setMessageCallback(
            [w](const TcpConnectionPtr&, Buffer* buf, Timestamp) {
                if (auto self = w.lock()) self->onRead(buf);
            });
        client->connect();
    }

    void onConnect(const TcpConnectionPtr& c) {
        if (stopped.load() || !c->connected()) {
            if (c->disconnected()) scheduleReconnect();
            return;
        }
        conn = c;
        state = State::kHandshake;
        handshakeBytes = 0;
        // C0 + C1（简单握手，随机 1536）
        handshakeC1.resize(1536);
        std::random_device rd;
        for (auto& b : handshakeC1) b = static_cast<uint8_t>(rd() & 0xFF);
        std::string out;
        out.push_back('\x03');  // C0 version 3
        out.append(reinterpret_cast<char*>(handshakeC1.data()), 1536);
        c->send(out);
    }

    void scheduleReconnect() {
        if (stopped.load()) return;
        conn.reset();
        state = State::kHandshake;
        reconnects.fetch_add(1);
        int wait = backoffMs;
        backoffMs = std::min(backoffMs * 2, 10000);
        auto w = weak_from_this();
        loop->runAfter(static_cast<double>(wait) / 1000.0, [w] {
            if (auto self = w.lock()) {
                if (!self->stopped.load()) self->client->connect();
            }
        });
    }

    void onRead(Buffer* buf) {
        if (state == State::kHandshake) {
            // 期望 S0(1)+S1(1536)+S2(1536) = 3073
            readBuf.append(buf->peek(), buf->readableBytes());
            buf->retrieveAll();
            size_t need = 1 + 1536 + 1536;
            if (readBuf.readableBytes() < need) return;
            // 发 C2 = 回显 S1
            const char* p = readBuf.peek();
            std::string c2(p + 1, 1536);
            if (conn) conn->send(c2);
            readBuf.retrieve(need);
            backoffMs = 500;
            sendConnect();
            state = State::kConnectSent;
            // 把剩余字节交给 chunk parser
            if (readBuf.readableBytes() > 0) {
                Buffer rest;
                rest.append(readBuf.peek(), readBuf.readableBytes());
                readBuf.retrieveAll();
                parseChunks(rest);
            }
            return;
        }
        parseChunks(*buf);
    }

    void parseChunks(Buffer& buf) {
        while (buf.readableBytes() > 0) {
            RtmpMessage msg;
            if (!parser.parseMessage(buf, msg)) break;
            handleMessage(msg);
        }
    }

    void handleMessage(const RtmpMessage& msg) {
        if (msg.typeId == kMsgSetChunkSize && msg.payload.size() >= 4) {
            serverChunkSize = (static_cast<uint32_t>(msg.payload[0]) << 24) |
                              (static_cast<uint32_t>(msg.payload[1]) << 16) |
                              (static_cast<uint32_t>(msg.payload[2]) << 8) |
                              msg.payload[3];
            parser.setChunkSize(serverChunkSize);
            return;
        }
        if (msg.typeId == kMsgWindowAckSize ||
            msg.typeId == kMsgSetPeerBandwidth ||
            msg.typeId == kMsgUserControlMessage ||
            msg.typeId == kMsgAcknowledgement) {
            return;  // 忽略控制消息
        }
        if (msg.typeId == kMsgCommandAmf0) {
            Buffer pb;
            pb.append(reinterpret_cast<const char*>(msg.payload.data()),
                      msg.payload.size());
            Amf0Value name;
            if (!name.decode(pb)) return;
            std::string cmd = name.asString();
            Amf0Value txn;
            txn.decode(pb);
            if (cmd == "_result" || cmd == "onStatus") {
                Amf0Value props;
                props.decode(pb);
                Amf0Value info;
                if (info.decode(pb) && info.isObject()) {
                    const auto* obj = info.asObject();
                    auto desc = obj->find("description");
                    if (desc != obj->end())
                        LOG_INFO("forward[%lu] %s: %s",
                                 static_cast<unsigned long>(id), cmd.c_str(),
                                 desc->second.asString().c_str());
                }
                if (state == State::kConnectSent) {
                    sendCreateStream();
                    state = State::kCreateSent;
                } else if (state == State::kCreateSent) {
                    // createStream 返回 stream id（number 在 info 中）
                    gotCreateResult = true;
                    sendPublish();
                    state = State::kPublishSent;
                } else if (state == State::kPublishSent) {
                    state = State::kPublishing;
                    LOG_INFO("forward[%lu] publishing to %s",
                             static_cast<unsigned long>(id), urlStr.c_str());
                    flushQueue();
                }
            } else if (cmd == "_error") {
                LOG_WARN("forward[%lu] RTMP error, reconnect",
                         static_cast<unsigned long>(id));
                if (conn) conn->shutdown();
            }
        }
    }

    void sendConnect() {
        std::string tcUrl = std::string(url.tls ? "rtmps://" : "rtmp://") +
                            url.host + ":" + std::to_string(url.port) + "/" +
                            url.app;
        Amf0Value::Object props;
        props["app"] = Amf0Value::str(url.app);
        props["flashVer"] = Amf0Value::str("FMLE/3.0 (compatible; FMSc/1.0)");
        props["tcUrl"] = Amf0Value::str(tcUrl);
        props["fpad"] = Amf0Value::boolean(false);
        props["capabilities"] = Amf0Value::number(15.0);
        props["audioCodecs"] = Amf0Value::number(4071.0);
        props["videoCodecs"] = Amf0Value::number(252.0);
        props["videoFunction"] = Amf0Value::number(1.0);
        props["objectEncoding"] = Amf0Value::number(0.0);
        std::vector<Amf0Value> args = {Amf0Value::object(props)};
        Buffer out;
        writeCommand(out, "connect", nextTxnId++, args, 0, kCsIdInvoke);
        sendBuffer(out);
    }

    void sendCreateStream() {
        std::vector<Amf0Value> args = {Amf0Value::null()};
        Buffer out;
        writeCommand(out, "createStream", nextTxnId++, args, 0, kCsIdInvoke);
        sendBuffer(out);
    }

    void sendPublish() {
        // releaseStream / FCPublish / publish
        Buffer out;
        writeCommand(out, "releaseStream", nextTxnId++,
                     {Amf0Value::null(), Amf0Value::str(url.key)}, 0,
                     kCsIdInvoke);
        writeCommand(out, "FCPublish", nextTxnId++,
                     {Amf0Value::null(), Amf0Value::str(url.key)}, 0,
                     kCsIdInvoke);
        writeCommand(out, "publish", nextTxnId++,
                     {Amf0Value::null(), Amf0Value::str(url.key),
                      Amf0Value::str("live")},
                     1, kCsIdInvoke);
        sendBuffer(out);
    }

    void sendMetadata(const MediaPacket& pkt) {
        // 透传源流的 script tag（onMetaData），直接用原 payload
        if (!conn || !pkt.data.empty()) {
            RtmpMessage m;
            m.typeId = kMsgDataAmf0;
            m.chunkStreamId = kCsIdData;
            m.streamId = 1;
            m.timestamp = pkt.timestamp;
            m.payload = pkt.data;
            Buffer out;
            ChunkWriter w;
            w.write(out, m);
            sendBuffer(out);
        }
    }

    void sendMedia(const MediaPacket& pkt) {
        if (state != State::kPublishing || !conn) {
            if (queue.size() < kMaxQueued) queue.push_back(pkt);
            return;
        }
        RtmpMessage m;
        m.typeId = (pkt.type == MediaPacket::kVideo) ? kMsgVideo : kMsgAudio;
        m.chunkStreamId =
            (pkt.type == MediaPacket::kVideo) ? kCsIdVideo : kCsIdAudio;
        m.streamId = 1;
        m.timestamp = pkt.timestamp;
        m.payload = pkt.data;
        Buffer out;
        ChunkWriter w;
        w.write(out, m);
        sendBuffer(out);
    }

    void flushQueue() {
        while (!queue.empty()) {
            MediaPacket p = std::move(queue.front());
            queue.pop_front();
            if (p.type == MediaPacket::kScript)
                sendMetadata(p);
            else
                sendMedia(p);
        }
    }

    void sendBuffer(Buffer& buf) {
        if (conn && buf.readableBytes() > 0) {
            bytesSent.fetch_add(buf.readableBytes());
            conn->send(buf.retrieveAllAsString());
        }
    }

    void stop() {
        stopped.store(true);
        if (conn) conn->shutdown();
        if (client) client->disconnect();
    }
};

// ---- StreamForwarder ----

StreamForwarder::StreamForwarder(EventLoop* loop,
                                 std::shared_ptr<MediaStream> stream)
    : loop_(loop), stream_(std::move(stream)) {}

StreamForwarder::~StreamForwarder() { stop(); }

uint64_t StreamForwarder::addTarget(const std::string& url) {
    RtmpUrl u;
    if (!parseRtmpUrl(url, u)) {
        LOG_WARN("StreamForwarder: bad url %s", url.c_str());
        return 0;
    }
    uint64_t id = nextId_.fetch_add(1);
    auto t = std::make_shared<Target>(id, u, url, loop_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        targets_[id] = t;
    }
    if (running_.load()) {
        loop_->runInLoop([t] { t->start(); });
    }
    return id;
}

void StreamForwarder::removeTarget(uint64_t id) {
    std::shared_ptr<Target> t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = targets_.find(id);
        if (it == targets_.end()) return;
        t = it->second;
        targets_.erase(it);
    }
    loop_->runInLoop([t] { t->stop(); });
}

void StreamForwarder::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    std::vector<std::shared_ptr<Target>> ts;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : targets_) ts.push_back(kv.second);
    }
    for (auto& t : ts) loop_->runInLoop([t] { t->start(); });
    subId_ = stream_->subscribe(
        [this](const MediaPacket& pkt) { onPacket(pkt); });
}

void StreamForwarder::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (stream_ && subId_ != 0) {
        stream_->unsubscribe(subId_);
        subId_ = 0;
    }
    std::vector<std::shared_ptr<Target>> ts;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : targets_) ts.push_back(kv.second);
    }
    for (auto& t : ts) loop_->runInLoop([t] { t->stop(); });
}

void StreamForwarder::onPacket(const MediaPacket& pkt) {
    std::vector<std::shared_ptr<Target>> ts;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : targets_) ts.push_back(kv.second);
    }
    for (auto& t : ts) {
        loop_->runInLoop([t, pkt] {
            if (pkt.type == MediaPacket::kScript)
                t->sendMetadata(pkt);
            else
                t->sendMedia(pkt);
        });
    }
}

size_t StreamForwarder::targetCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return targets_.size();
}

std::vector<StreamForwarder::Status> StreamForwarder::status() const {
    std::vector<Status> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : targets_) {
        Status s;
        s.id = kv.second->id;
        s.url = kv.second->urlStr;
        s.connected = kv.second->state == Target::State::kPublishing;
        s.reconnects = kv.second->reconnects.load();
        s.bytesSent = kv.second->bytesSent.load();
        out.push_back(s);
    }
    return out;
}

}  // namespace streaming
