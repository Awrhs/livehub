#include "rtmp/RtmpServer.hpp"

#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "net/TcpConnection.hpp"
#include "rtmp/RtmpConnection.hpp"

#include <mutex>
#include <unordered_map>

namespace streaming {

namespace {
using ConnMap =
    std::unordered_map<const TcpConnection*, std::shared_ptr<RtmpConnection>>;
}  // namespace

RtmpServer::RtmpServer(EventLoop* loop, uint16_t port, std::string app)
    : server_(loop, port, "0.0.0.0", TcpServer::kReusePort),
      app_(std::move(app)) {
    auto conns = std::make_shared<ConnMap>();
    auto self = this;

    server_.setConnectionCallback(
        [this, conns](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                auto rc = std::make_shared<RtmpConnection>(conn, app_);
                rc->setPublishCallback(
                    [this, conns](const std::shared_ptr<RtmpConnection>& s,
                                  const std::string& app,
                                  const std::string& key,
                                  std::shared_ptr<MediaStream> ms) {
                        {
                            std::lock_guard<std::mutex> lk(streamsMu_);
                            streams_[key] = ms;
                        }
                        if (onPublish_) onPublish_(ms);
                        (void)s;
                        (void)app;
                    });
                rc->setPlayCallback(
                    [this](const std::shared_ptr<RtmpConnection>& c,
                           const std::string& app, const std::string& key) {
                        if (onPlay_) onPlay_(c, app, key);
                    });
                rc->setAuthCallback(
                    [this](const std::string& host, const std::string& app,
                           const std::string& key, bool publish,
                           std::string* reason) {
                        if (onAuth_) return onAuth_(host, app, key, publish,
                                                    reason);
                        return true;
                    });
                rc->setCloseCallback(
                    [this, conns](const std::shared_ptr<RtmpConnection>& c) {
                        if (c->isPublisher() && c->stream()) {
                            removeStream(c->streamKey());
                        }
                        // 回收该连接占用的 vhost 配额
                        auto vh = resolveVhost(c->host());
                        if (vh) {
                            if (c->isPublisher()) vh->publishers.fetch_sub(1);
                            if (c->isPlayer()) vh->viewers.fetch_sub(1);
                        }
                        if (auto tc = c->tcpConn()) {
                            conns->erase(tc.get());
                        }
                    });
                {
                    // 连接建立后注册；conns 在连接回调线程（ioLoop）访问，
                    // 但同一 TcpConnection 的消息/关闭都在同一 loop，安全。
                    (*conns)[conn.get()] = rc;
                }
                LOG_INFO("RTMP connection up: %s", conn->name().c_str());
            } else {
                LOG_INFO("RTMP connection down: %s", conn->name().c_str());
                // 异常断连：触发与 deleteStream 相同的清理（移除流、回收配额）
                auto it = conns->find(conn.get());
                if (it != conns->end()) {
                    auto c = it->second;
                    conns->erase(it);
                    if (c->isPublisher() && c->stream()) {
                        removeStream(c->streamKey());
                    }
                    auto vh = resolveVhost(c->host());
                    if (vh) {
                        if (c->isPublisher()) vh->publishers.fetch_sub(1);
                        if (c->isPlayer()) vh->viewers.fetch_sub(1);
                    }
                }
            }
        });

    server_.setMessageCallback(
        [conns](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
            auto it = conns->find(conn.get());
            if (it != conns->end()) {
                it->second->onRead(buf);
            }
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

void RtmpServer::removeStream(const std::string& key) {
    UnpublishHandler cb;
    {
        std::lock_guard<std::mutex> lk(streamsMu_);
        auto it = streams_.find(key);
        if (it != streams_.end()) {
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
