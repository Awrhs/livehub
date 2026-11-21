#include "rtmp/RtmpServer.hpp"

#include <any>

#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "net/TcpConnection.hpp"
#include "rtmp/RtmpConnection.hpp"

#include <mutex>
#include <unordered_map>

namespace streaming {

namespace {
std::shared_ptr<RtmpConnection> sessionOf(const TcpConnectionPtr& conn) {
    const std::any& ctx = conn->getContext();
    if (auto p = std::any_cast<std::shared_ptr<RtmpConnection>>(&ctx)) return *p;
    return nullptr;
}
}  // namespace

namespace {
}  // namespace

RtmpServer::RtmpServer(EventLoop* loop, uint16_t port, std::string app)
    : server_(loop, port, "0.0.0.0", TcpServer::kReusePort),
      app_(std::move(app)) {

    server_.setConnectionCallback(
        [this](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                auto rc = std::make_shared<RtmpConnection>(conn, app_);
                rc->setPublishCallback(
                    [this](const std::shared_ptr<RtmpConnection>& c,
                           const std::string&, const std::string& key,
                           std::shared_ptr<MediaStream> ms) -> bool {
                        {
                            // 原子占用流名：已有推流者则拒绝（原实现直接覆盖）
                            std::lock_guard<std::mutex> lk(streamsMu_);
                            auto it = streams_.find(key);
                            if (it != streams_.end() && it->second->hasPublisher())
                                return false;
                            streams_[key] = ms;
                        }
                        // 推流成功才计入配额（关闭时按 quotaCounted 回收）
                        if (auto vh = resolveVhost(c->host())) {
                            vh->publishers.fetch_add(1);
                            c->setQuotaCounted(true);
                        }
                        if (onPublish_) onPublish_(ms);
                        return true;
                    });
                rc->setPlayCallback(
                    [this](const std::shared_ptr<RtmpConnection>& c,
                           const std::string& app, const std::string& key) {
                        // 原实现只回 Play.Start 而从不下发媒体数据
                        auto ms = findStream(key);
                        if (!ms) {
                            c->rejectPlay("NetStream.Play.StreamNotFound",
                                          "stream not found: " + key);
                            return;
                        }
                        c->startPlayback(ms);
                        if (auto vh = resolveVhost(c->host())) {
                            vh->viewers.fetch_add(1);
                            c->setQuotaCounted(true);
                        }
                        if (onPlay_) onPlay_(c, app, key);
                    });
                rc->setAuthCallback(
                    [this](const std::string& host, const std::string& app,
                           const std::string& key, bool publish,
                           RtmpConnection::AuthDone done) {
                        if (onAuth_) onAuth_(host, app, key, publish, std::move(done));
                        else done(true, "");
                    });
                rc->setCloseCallback(
                    [this](const std::shared_ptr<RtmpConnection>& c) {
                        if (c->isPublisher() && c->stream()) {
                            removeStream(c->streamKey(), c->stream());
                        }
                        // 回收该连接占用的 vhost 配额
                        auto vh = resolveVhost(c->host());
                        if (vh && c->quotaCounted()) {
                            if (c->isPublisher()) vh->publishers.fetch_sub(1);
                            if (c->isPlayer()) vh->viewers.fetch_sub(1);
                            c->setQuotaCounted(false);
                        }
                        // 解除 TcpConnection 对会话的持有；断连回调据此不再重复清理
                        if (auto tc = c->tcpConn()) tc->setContext(std::any());
                    });
                // 会话挂在各自 TcpConnection 的 context 上：每个连接只在自己的
                // IO 线程访问，无需全局共享表（原全局 map 被多个 IO 线程并发读写）。
                conn->setContext(rc);
                LOG_INFO("RTMP connection up: %s", conn->name().c_str());
            } else {
                LOG_INFO("RTMP connection down: %s", conn->name().c_str());
                // 异常断连：触发与 deleteStream 相同的清理（移除流、回收配额）
                auto c = sessionOf(conn);
                if (c) {
                    conn->setContext(std::any());
                    if (c->isPublisher() && c->stream()) {
                        removeStream(c->streamKey(), c->stream());
                    }
                    auto vh = resolveVhost(c->host());
                    if (vh && c->quotaCounted()) {
                        if (c->isPublisher()) vh->publishers.fetch_sub(1);
                        if (c->isPlayer()) vh->viewers.fetch_sub(1);
                        c->setQuotaCounted(false);
                    }
                }
            }
        });

    server_.setMessageCallback(
        [](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
            // 持有一份引用：onRead 内部可能 close() 并释放 context 中的引用，
            // 若不保活，RtmpConnection 会在自身成员函数执行中途被析构（UAF）。
            auto rc = sessionOf(conn);
            if (rc) rc->onRead(buf);
        });
}

RtmpServer::~RtmpServer() = default;

void RtmpServer::start() {
    LOG_INFO("RTMP server starting (app=%s)", app_.c_str());
    server_.start();
}

std::shared_ptr<MediaStream> RtmpServer::findStream(
    const std::string& key) const {
    std::lock_guard<std::mutex> lk(streamsMu_);
    auto it = streams_.find(key);
    return it == streams_.end() ? nullptr : it->second;
}

void RtmpServer::removeStream(const std::string& key,
                              const std::shared_ptr<MediaStream>& owner) {
    UnpublishHandler cb;
    {
        std::lock_guard<std::mutex> lk(streamsMu_);
        auto it = streams_.find(key);
        // owner 非空时只移除属于该推流者的流，避免误删同名的新流
        if (it != streams_.end() && (!owner || it->second == owner)) {
            it->second->setHasPublisher(false);
            streams_.erase(it);
            LOG_INFO("RTMP stream removed: %s", key.c_str());
            cb = onUnpublish_;
        }
    }
    // 在锁外通知，避免回调中反查 RtmpServer 造成死锁
    if (cb) cb(app_, key);
}

size_t RtmpServer::streamCount() const {
    std::lock_guard<std::mutex> lk(streamsMu_);
    return streams_.size();
}

std::shared_ptr<MediaStream> RtmpServer::getOrCreateStream(
    const std::string& key) {
    std::lock_guard<std::mutex> lk(streamsMu_);
    auto it = streams_.find(key);
    if (it != streams_.end()) return it->second;
    auto ms = std::make_shared<MediaStream>(key, app_);
    streams_[key] = ms;
    if (onPublish_) onPublish_(ms);
    return ms;
}

std::vector<std::string> RtmpServer::listKeys() const {
    std::lock_guard<std::mutex> lk(streamsMu_);
    std::vector<std::string> keys;
    keys.reserve(streams_.size());
    for (const auto& kv : streams_) keys.push_back(kv.first);
    return keys;
}

}  // namespace streaming
