#include "cluster/OriginEdge.hpp"

#include "core/Logger.hpp"
#include "net/EventLoop.hpp"
#include "net/TcpConnection.hpp"
#include "net/Timer.hpp"

namespace streaming {

struct OriginEdge::PullSession
    : public std::enable_shared_from_this<PullSession> {
    std::string key;
    std::string host;
    uint16_t port;
    std::string app;
    std::shared_ptr<MediaStream> stream;
    std::unique_ptr<TcpClient> client;
    FlvDemuxer demuxer;
    std::atomic<int> refCount{0};
    // HTTP 响应头解析状态
    bool headerParsed_ = false;
    size_t contentLength_ = 0;
    bool chunked_ = false;
    // 延迟释放
    EventLoop* loop;
    TimerId releaseTimer;  // 空 TimerId 表示无待释放
    static constexpr double kReleaseDelaySec = 5.0;

    PullSession(EventLoop* l, std::string k, std::string h,
                uint16_t p, std::string a)
        : key(std::move(k)), host(std::move(h)), port(p), app(std::move(a)),
          loop(l) {
        stream = std::make_shared<MediaStream>(key, app);
        stream->setHasPublisher(true);
    }

    // 必须在对象被 shared_ptr 接管后调用：构造函数中调用 shared_from_this()
    // 会抛 std::bad_weak_ptr（原实现边缘节点首次回源即 abort）
    void start() {
        client = std::make_unique<TcpClient>(loop, host, port,
                                              "edge-" + key);
        client->enableRetry(true);  // 源站断开时自动重连（指数退避）
        std::weak_ptr<PullSession> weak = shared_from_this();
        client->setConnectionCallback(
            [weak](const TcpConnectionPtr& conn) {
                auto self = weak.lock();
                if (!self) return;
                if (!conn->connected()) {
                    self->headerParsed_ = false;
                    self->chunked_ = false;
                    self->contentLength_ = 0;
                    return;
                }
                // 每次（重）连接都重置 demuxer 并重新发起 GET
                self->demuxer.reset();
                self->headerParsed_ = false;
                self->chunked_ = false;
                // 捕获 weak 引用，避免 demuxer 回调强持有 self 造成循环引用
                self->demuxer.setTagCallback([weak](const MediaPacket& pkt) {
                    auto self = weak.lock();
                    if (!self) return;
                    if (pkt.type == MediaPacket::kVideo &&
                        pkt.isSequenceHeader) {
                        self->stream->setVideoSequenceHeader(pkt.data);
                    } else if (pkt.type == MediaPacket::kAudio &&
                               pkt.isSequenceHeader) {
                        self->stream->setAudioSequenceHeader(pkt.data);
                    }
                    self->stream->onPacket(pkt);
                });
                std::string req =
                    "GET /live/" + self->key + ".flv HTTP/1.1\r\nHost: " +
                    self->host + ":" + std::to_string(self->port) +
                    "\r\nUser-Agent: streaming-edge/1.0\r\n"
                    "Accept: */*\r\nConnection: keep-alive\r\n\r\n";
                conn->send(req);
            });
        client->setMessageCallback(
            [weak](const TcpConnectionPtr&, Buffer* buf, Timestamp) {
                auto self = weak.lock();
                if (!self) return;
                if (!self->headerParsed_) {
                    self->parseHttpHeader(*buf);
                    if (!self->headerParsed_) return;
                }
                if (buf->readableBytes() == 0) return;
                if (self->chunked_) {
                    self->feedChunked(*buf);
                } else {
                    self->demuxer.feed(
                        reinterpret_cast<const uint8_t*>(buf->peek()),
                        buf->readableBytes());
                    buf->retrieveAll();
                }
            });
        client->connect();
    }

    ~PullSession() {
        if (client) client->disconnect();
        stream->setHasPublisher(false);
    }

    void parseHttpHeader(Buffer& buf) {
        const char* data = buf.peek();
        size_t len = buf.readableBytes();
        size_t end = 0;
        for (size_t i = 0; i + 3 < len; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n' &&
                data[i + 2] == '\r' && data[i + 3] == '\n') {
                end = i + 4;
                break;
            }
        }
        if (end == 0) return;
        std::string header(data, end);
        if (header.find("Transfer-Encoding: chunked") != std::string::npos ||
            header.find("transfer-encoding: chunked") != std::string::npos) {
            chunked_ = true;
        }
        headerParsed_ = true;
        buf.retrieve(end);
    }

    // 极简 chunked 解码
    void feedChunked(Buffer& buf) {
        while (buf.readableBytes() > 0) {
            const char* p = buf.peek();
            size_t len = buf.readableBytes();
            size_t lineEnd = 0;
            for (size_t i = 0; i + 1 < len; ++i) {
                if (p[i] == '\r' && p[i + 1] == '\n') {
                    lineEnd = i + 2;
                    break;
                }
            }
            if (lineEnd == 0) return;
            std::string sizeStr(p, lineEnd - 2);
            size_t semicolon = sizeStr.find(';');
            if (semicolon != std::string::npos)
                sizeStr = sizeStr.substr(0, semicolon);
            size_t chunkSize =
                static_cast<size_t>(std::stoul(sizeStr, nullptr, 16));
            if (chunkSize == 0) {
                buf.retrieveAll();
                return;  // 结束
            }
            if (len < lineEnd + chunkSize + 2) return;  // 等更多数据
            demuxer.feed(reinterpret_cast<const uint8_t*>(p + lineEnd),
                         chunkSize);
            buf.retrieve(lineEnd + chunkSize + 2);
        }
    }

    void acquire() {
        refCount.fetch_add(1);
        // 取消待释放
        if (releaseTimer && loop) {
            loop->cancel(releaseTimer);
            releaseTimer = TimerId();
        }
    }

    // release 后延迟真正销毁，避免抖动重连
    void scheduleRelease(std::function<void()> destroy) {
        refCount.fetch_sub(1);
        if (refCount.load() > 0) return;
        std::weak_ptr<PullSession> weak = shared_from_this();
        releaseTimer =
            loop->runAfter(kReleaseDelaySec, [weak, destroy] {
                if (auto self = weak.lock()) {
                    if (self->refCount.load() <= 0) destroy();
                    self->releaseTimer = TimerId();
                }
            });
    }
};

OriginEdge::OriginEdge(EventLoop* loop, std::string originHost,
                       uint16_t originPort, std::string app)
    : loop_(loop),
      originHost_(std::move(originHost)),
      originPort_(originPort),
      app_(std::move(app)) {}

OriginEdge::~OriginEdge() = default;

std::shared_ptr<MediaStream> OriginEdge::getOrPull(
    const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        it->second->acquire();
        return it->second->stream;
    }
    auto sess = std::make_shared<PullSession>(loop_, key, originHost_,
                                               originPort_, app_);
    sess->start();
    sess->acquire();
    sessions_[key] = sess;
    LOG_INFO("OriginEdge: start pull %s from %s:%u", key.c_str(),
             originHost_.c_str(), originPort_);
    return sess->stream;
}

void OriginEdge::release(const std::string& key) {
    std::shared_ptr<PullSession> sess;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(key);
        if (it == sessions_.end()) return;
        sess = it->second;
    }
    EventLoop* loop = loop_;
    sess->scheduleRelease([this, key, loop] {
        // 在 loop 线程销毁
        loop->runInLoop([this, key] {
            std::lock_guard<std::mutex> lk(mu_);
            sessions_.erase(key);
            LOG_INFO("OriginEdge: stop pull %s", key.c_str());
        });
    });
}

size_t OriginEdge::activePulls() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}

}  // namespace streaming
