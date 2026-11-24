#include "srt/SrtServer.hpp"

#ifdef HAVE_LIBSRT
#include <arpa/inet.h>
#include <srt/srt.h>
// srt.h 间接包含 <syslog.h>，其 LOG_INFO/LOG_DEBUG 等常量会与本项目日志宏冲突
#undef LOG_INFO
#undef LOG_DEBUG
#endif

#include "core/Logger.hpp"
#include "hls/FlvToTs.hpp"
#include "hls/TsDemuxer.hpp"
#include "hls/TsMuxer.hpp"

#include <chrono>
#include <cstring>
#include <vector>

namespace streaming {

struct SrtServer::Session {
    int sock = -1;
    bool publish = false;
    std::string key;
    std::shared_ptr<MediaStream> stream;
    std::atomic<bool> closed{false};
    // 推流
    std::unique_ptr<TsDemuxer> demux;
    // 拉流
    std::unique_ptr<TsMuxer> muxer;
    std::unique_ptr<FlvToTs> conv;
    std::vector<uint8_t> agg;
    MediaStream::SubscriberId subId = 0;
    uint64_t dropped = 0;
    std::chrono::steady_clock::time_point noPublisherSince{};
};

bool SrtServer::parseStreamId(const std::string& sid, std::string* key,
                              std::string* rawKey, bool* publish) {
    std::string resource, mode, token;
    if (sid.compare(0, 4, "#!::") == 0) {
        // Access Control 语法：逗号分隔的 k=v
        std::string body = sid.substr(4);
        size_t p = 0;
        while (p <= body.size()) {
            size_t c = body.find(',', p);
            std::string kv = body.substr(p, c == std::string::npos ? std::string::npos : c - p);
            auto eq = kv.find('=');
            if (eq != std::string::npos) {
                std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                if (k == "r") resource = v;
                else if (k == "m") mode = v;
                else if (k == "t") token = v;
            }
            if (c == std::string::npos) break;
            p = c + 1;
        }
    } else {
        resource = sid;
    }
    if (resource.empty()) return false;
    // 去掉 app/ 前缀，保留查询串
    std::string query;
    auto q = resource.find('?');
    if (q != std::string::npos) {
        query = resource.substr(q + 1);
        resource = resource.substr(0, q);
    }
    auto slash = resource.rfind('/');
    if (slash != std::string::npos) resource = resource.substr(slash + 1);
    if (resource.empty() || resource.find("..") != std::string::npos) return false;
    if (!token.empty()) query += (query.empty() ? "" : "&") + std::string("token=") + token;
    *key = resource;
    *rawKey = query.empty() ? resource : resource + "?" + query;
    *publish = (mode == "publish");
    return true;
}

SrtServer::SrtServer(EventLoop*, uint16_t port, const std::string& ip)
    : port_(port), ip_(ip) {}

SrtServer::~SrtServer() {
    alive_->store(false);  // 尚未返回的异步鉴权回调不再访问本对象
    stop();
}

size_t SrtServer::sessionCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}

#ifndef HAVE_LIBSRT

void SrtServer::start() {
    LOG_WARN("SRT disabled: built without libsrt (install libsrt-openssl-dev)");
}
void SrtServer::stop() {}
void SrtServer::run() {}
void SrtServer::acceptOne() {}
void SrtServer::setupSession(int, const std::string&, bool) {}
void SrtServer::closeSession(int) {}

#else  // HAVE_LIBSRT

namespace {
constexpr int kLivePayload = 1316;  // 7 * 188

void setBool(int s, SRT_SOCKOPT opt, bool v) { srt_setsockflag(s, opt, &v, sizeof(v)); }
}  // namespace

void SrtServer::start() {
    if (running_.exchange(true)) return;
    srt_startup();
    listener_ = srt_create_socket();
    setBool(listener_, SRTO_RCVSYN, false);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port_);
    inet_pton(AF_INET, ip_.c_str(), &sa.sin_addr);
    if (srt_bind(listener_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SRT_ERROR ||
        srt_listen(listener_, 64) == SRT_ERROR) {
        LOG_ERROR("SRT listen on %s:%u failed: %s", ip_.c_str(), port_,
                  srt_getlasterror_str());
        srt_close(listener_);
        listener_ = -1;
        running_ = false;
        return;
    }
    eid_ = srt_epoll_create();
    int ev = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    srt_epoll_add_usock(eid_, listener_, &ev);
    thread_ = std::thread([this] { run(); });
    LOG_INFO("SRT server (libsrt %s) listening on udp/%u", SRT_VERSION_STRING, port_);
}

void SrtServer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    std::vector<int> socks;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : sessions_) socks.push_back(kv.first);
    }
    for (int s : socks) closeSession(s);
    if (eid_ >= 0) srt_epoll_release(eid_);
    if (listener_ >= 0) srt_close(listener_);
    eid_ = listener_ = -1;
    // 不调用 srt_cleanup()：libsrt 1.5.x 在进程退出的全局析构中会再次清理，
    // 提前 cleanup 会导致退出时段错误（Docker 冒烟测试捕获的崩溃栈）

}

void SrtServer::acceptOne() {
    sockaddr_storage peer{};
    int plen = sizeof(peer);
    int s = srt_accept(listener_, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (s == SRT_INVALID_SOCK) return;
    setBool(s, SRTO_RCVSYN, false);
    setBool(s, SRTO_SNDSYN, false);

    char sidBuf[512] = {0};
    int sidLen = sizeof(sidBuf);
    srt_getsockflag(s, SRTO_STREAMID, sidBuf, &sidLen);
    std::string sid(sidBuf, sidLen > 0 ? static_cast<size_t>(sidLen) : 0);

    std::string key, rawKey;
    bool publish = false;
    if (!parseStreamId(sid, &key, &rawKey, &publish)) {
        LOG_WARN("SRT reject: bad streamid '%s'", sid.c_str());
        srt_close(s);
        return;
    }
    if (!auth_) {
        setupSession(s, key, publish);
        return;
    }
    // 异步鉴权：结果回来前连接保持未登记状态（libsrt 内部缓冲推流数据）
    auto alive = alive_;
    auth_(rawKey, publish, [this, alive, s, key, publish](bool ok, const std::string& why) {
        if (!alive->load() || !running_.load()) {
            srt_close(s);
            return;
        }
        if (!ok) {
            LOG_WARN("SRT %s denied: key=%s reason=%s", publish ? "publish" : "play",
                     key.c_str(), why.c_str());
            srt_close(s);
            return;
        }
        setupSession(s, key, publish);
    });
}

void SrtServer::setupSession(int s, const std::string& key, bool publish) {
    auto sess = std::make_shared<Session>();
    sess->sock = s;
    sess->publish = publish;
    sess->key = key;

    if (publish) {
        sess->stream = resolver_ ? resolver_(key) : nullptr;
        if (!sess->stream) {
            LOG_WARN("SRT publish rejected (stream busy or not allowed): %s", key.c_str());
            srt_close(s);
            return;
        }
        // 踢流：closeSession 加锁且幂等，可在任意线程调用
        sess->stream->setPublisherCloser([this, s] { closeSession(s); });
        sess->demux = std::make_unique<TsDemuxer>();
        std::weak_ptr<MediaStream> wms = sess->stream;
        sess->demux->setPacketCallback([wms](const MediaPacket& pkt) {
            auto ms = wms.lock();
            if (!ms) return;
            if (pkt.isSequenceHeader) {
                if (pkt.type == MediaPacket::kVideo) ms->setVideoSequenceHeader(pkt.data);
                else if (pkt.type == MediaPacket::kAudio) ms->setAudioSequenceHeader(pkt.data);
            }
            ms->touch(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count());
            ms->onPacket(pkt);
        });
        int ev = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        srt_epoll_add_usock(eid_, s, &ev);
    } else {
        sess->stream = playResolver_ ? playResolver_(key) : nullptr;
        if (!sess->stream) {
            LOG_WARN("SRT play: stream not found: %s", key.c_str());
            srt_close(s);
            return;
        }
        Session* raw = sess.get();
        sess->muxer = std::make_unique<TsMuxer>([raw](const uint8_t* p, size_t n) {
            // 聚合为 1316 字节（7 个 TS 包）的 SRT 消息，非阻塞发送；发送缓冲满则丢弃
            raw->agg.insert(raw->agg.end(), p, p + n);
            if (raw->agg.size() >= static_cast<size_t>(kLivePayload)) {
                if (srt_sendmsg2(raw->sock, reinterpret_cast<const char*>(raw->agg.data()),
                                 static_cast<int>(raw->agg.size()), nullptr) == SRT_ERROR)
                    raw->dropped += raw->agg.size();
                raw->agg.clear();
            }
        });
        sess->conv = std::make_unique<FlvToTs>(*sess->muxer);
        // 回调强持有 session；关闭时 unsubscribe 释放
        sess->subId = sess->stream->subscribeWithGop([sess](const MediaPacket& pkt) {
            if (sess->closed.load()) return;
            sess->conv->onPacket(pkt);
        });
        // 拉流端也加入 epoll 以感知断开
        int ev = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        srt_epoll_add_usock(eid_, s, &ev);
    }
    LOG_INFO("SRT %s session up: key=%s sock=%d", publish ? "publish" : "play",
             key.c_str(), s);
    std::lock_guard<std::mutex> lk(mu_);
    sessions_[s] = std::move(sess);
}

void SrtServer::closeSession(int sock) {
    std::shared_ptr<Session> sess;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(sock);
        if (it == sessions_.end()) return;
        sess = it->second;
        sessions_.erase(it);
    }
    sess->closed = true;
    if (eid_ >= 0) srt_epoll_remove_usock(eid_, sock);
    if (sess->subId != 0 && sess->stream) sess->stream->unsubscribe(sess->subId);
    srt_close(sock);
    LOG_INFO("SRT %s session down: key=%s sock=%d dropped=%llu",
             sess->publish ? "publish" : "play", sess->key.c_str(), sock,
             static_cast<unsigned long long>(sess->dropped));
    if (sess->publish && onUnpublish_) onUnpublish_(sess->key, sess->stream);
}

void SrtServer::run() {
    std::vector<char> buf(1500);
    SRT_EPOLL_EVENT events[64];
    while (running_) {
        int n = srt_epoll_uwait(eid_, events, 64, 100);
        for (int i = 0; i < n; ++i) {
            int s = events[i].fd;
            if (s == listener_) {
                acceptOne();
                continue;
            }
            std::shared_ptr<Session> sess;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = sessions_.find(s);
                if (it != sessions_.end()) sess = it->second;
            }
            if (!sess) continue;
            bool broken = (events[i].events & SRT_EPOLL_ERR) != 0;
            // 读空所有可读消息
            while (!broken) {
                int r = srt_recvmsg(s, buf.data(), static_cast<int>(buf.size()));
                if (r > 0) {
                    if (sess->publish && sess->demux)
                        sess->demux->feed(reinterpret_cast<const uint8_t*>(buf.data()),
                                          static_cast<size_t>(r));
                    continue;
                }
                if (r == SRT_ERROR && srt_getlasterror(nullptr) == SRT_EASYNCRCV) break;
                broken = true;
            }
            SRT_SOCKSTATUS st = srt_getsockstate(s);
            if (broken || st == SRTS_BROKEN || st == SRTS_CLOSED || st == SRTS_NONEXIST)
                closeSession(s);
        }
        // 拉流会话：流已无发布者超过 3 秒则关闭
        std::vector<int> stale;
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [s, sess] : sessions_) {
                if (sess->publish) continue;
                if (sess->stream->hasPublisher()) {
                    sess->noPublisherSince = {};
                } else if (sess->noPublisherSince == std::chrono::steady_clock::time_point{}) {
                    sess->noPublisherSince = now;
                } else if (now - sess->noPublisherSince > std::chrono::seconds(3)) {
                    stale.push_back(s);
                }
            }
        }
        for (int s : stale) closeSession(s);
    }
}

#endif  // HAVE_LIBSRT

}  // namespace streaming
