#include "webrtc/WebRtcManager.hpp"
#include "core/Logger.hpp"

#include <random>
#include <sstream>
#include <vector>

namespace streaming {

namespace {
std::string randomToken(size_t len) {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static thread_local std::mt19937 rng(std::random_device{}());
    std::string s;
    s.reserve(len);
    std::uniform_int_distribution<size_t> d(0, sizeof(alphabet) - 2);
    for (size_t i = 0; i < len; ++i) s.push_back(alphabet[d(rng)]);
    return s;
}
}  // namespace

WebRtcManager::WebRtcManager(EventLoop* loop, uint16_t portBase,
                             uint16_t portRange)
    : loop_(loop),
      portBase_(portBase),
      portRange_(portRange),
      nextPort_(portBase),
      localFingerprint_(DtlsTransport::localFingerprintSha256()) {}

WebRtcManager::~WebRtcManager() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : sessions_) kv.second->stop();
    for (auto& kv : ingests_) kv.second->stop();
    sessions_.clear();
    ingests_.clear();
}

uint16_t WebRtcManager::allocPort() {
    // 轮询分配端口；真正可用性由 IceAgent bind 时决定。
    uint16_t p = nextPort_.fetch_add(1);
    if (p >= portBase_ + portRange_) {
        nextPort_ = portBase_;
        p = portBase_;
    }
    return p;
}

std::shared_ptr<WebRtcTransport> WebRtcManager::createSubscriber(
    const std::string& streamKey, std::shared_ptr<MediaStream> stream) {
    if (!stream) return nullptr;
    std::string sessionId = streamKey + "-" + randomToken(8);
    std::string ufrag = randomToken(4);
    std::string pwd = randomToken(22);

    uint16_t port = allocPort();
    auto transport = std::make_shared<WebRtcTransport>(
        loop_, port, ufrag, pwd, localFingerprint_, stream);
    transport->start();

    {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_[sessionId] = transport;
    }
    LOG_INFO("WebRtcManager: WHEP session %s on UDP %u", sessionId.c_str(),
             transport->port());
    return transport;
}

std::shared_ptr<WebRtcIngest> WebRtcManager::createIngest(
    const std::string& streamKey, std::shared_ptr<MediaStream> stream) {
    if (!stream) return nullptr;
    std::string sessionId = "whip-" + streamKey + "-" + randomToken(8);
    std::string ufrag = randomToken(4);
    std::string pwd = randomToken(22);
    uint16_t port = allocPort();
    auto ingest = std::make_shared<WebRtcIngest>(
        loop_, port, ufrag, pwd, localFingerprint_, stream);
    ingest->start();
    {
        std::lock_guard<std::mutex> lk(mu_);
        ingests_[sessionId] = ingest;
    }
    return ingest;
}

size_t WebRtcManager::sweepIdle(int64_t timeoutMs) {
    std::vector<std::shared_ptr<WebRtcTransport>> dead;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second->idle(timeoutMs)) {
                dead.push_back(it->second);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 原实现从不回收 WHEP 会话：观众关闭页面后 UDP 端口与订阅永久泄漏
    for (auto& t : dead) t->stop();
    if (!dead.empty())
        LOG_INFO("WebRtcManager: 回收 %zu 个空闲 WHEP 会话", dead.size());
    return dead.size();
}

size_t WebRtcManager::sessionCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}

void WebRtcManager::remove(const std::string& sessionId) {
    std::shared_ptr<WebRtcTransport> t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            t = it->second;
            sessions_.erase(it);
        }
    }
    if (t) t->stop();
}

}  // namespace streaming
