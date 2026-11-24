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
                             uint16_t portRange, uint16_t muxPort)
    : loop_(loop),
      portBase_(portBase),
      portRange_(portRange),
      nextPort_(portBase),
      localFingerprint_(DtlsTransport::localFingerprintSha256()) {
    if (muxPort != 0) {
        mux_ = std::make_unique<IceMux>(loop, muxPort);
        mux_->start();
    }
}

WebRtcManager::~WebRtcManager() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : sessions_) kv.second->stop();
    for (auto& kv : ingests_) kv.second->stop();
    sessions_.clear();
    ingests_.clear();
}

uint16_t WebRtcManager::allocPort() {
    if (mux_) return mux_->port();
    // 轮询分配端口；真正可用性由 IceAgent bind 时决定。
    uint16_t p = nextPort_.fetch_add(1);
    if (p >= portBase_ + portRange_) {
        nextPort_ = portBase_;
        p = portBase_;
    }
    return p;
}

std::shared_ptr<WebRtcTransport> WebRtcManager::createSubscriber(
    const std::string& streamKey, std::shared_ptr<MediaStream> stream, std::string* sessionIdOut) {
    if (!stream) return nullptr;
    // 会话 ID 同时是 DELETE 结束会话的凭据：足够长的随机串
    std::string sessionId = randomToken(20);
    if (sessionIdOut) *sessionIdOut = sessionId;
    std::string ufrag = randomToken(8);  // 单端口复用按 ufrag 分发，需足够长避免冲突
    std::string pwd = randomToken(22);

    uint16_t port = allocPort();
    auto transport = std::make_shared<WebRtcTransport>(
        loop_, port, ufrag, pwd, localFingerprint_, stream, mux_.get());
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
    const std::string& streamKey, std::shared_ptr<MediaStream> stream, std::string* sessionIdOut) {
    if (!stream) return nullptr;
    std::string sessionId = randomToken(20);
    if (sessionIdOut) *sessionIdOut = sessionId;
    std::string ufrag = randomToken(8);  // 单端口复用按 ufrag 分发，需足够长避免冲突
    std::string pwd = randomToken(22);
    uint16_t port = allocPort();
    auto ingest = std::make_shared<WebRtcIngest>(
        loop_, port, ufrag, pwd, localFingerprint_, stream, mux_.get());
    ingest->start();
    {
        std::lock_guard<std::mutex> lk(mu_);
        ingests_[sessionId] = ingest;
        ingestInfo_[sessionId] = IngestInfo{streamKey, stream};
    }
    return ingest;
}

size_t WebRtcManager::sweepIdle(int64_t timeoutMs) {
    std::vector<std::shared_ptr<WebRtcTransport>> dead;
    std::vector<std::pair<std::shared_ptr<WebRtcIngest>, IngestInfo>> deadIngests;
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
        // WHIP 推流端断开（浏览器关闭）：原实现从不回收，流名一直被占用
        for (auto it = ingests_.begin(); it != ingests_.end();) {
            if (it->second->idle(timeoutMs)) {
                deadIngests.emplace_back(it->second, ingestInfo_[it->first]);
                ingestInfo_.erase(it->first);
                it = ingests_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 原实现从不回收 WHEP 会话：观众关闭页面后 UDP 端口与订阅永久泄漏
    for (auto& t : dead) t->stop();
    for (auto& [ing, info] : deadIngests) endIngest(ing, info);
    if (!dead.empty() || !deadIngests.empty())
        LOG_INFO("WebRtcManager: 回收空闲会话 WHEP %zu 个、WHIP %zu 个", dead.size(), deadIngests.size());
    return dead.size() + deadIngests.size();
}

void WebRtcManager::endIngest(const std::shared_ptr<WebRtcIngest>& ingest, const IngestInfo& info) {
    ingest->stop();
    auto ms = info.stream.lock();
    if (ms) ms->setHasPublisher(false);
    if (onIngestEnd_ && ms) onIngestEnd_(info.key, ms);
}

bool WebRtcManager::removeSession(const std::string& sessionId) {
    std::shared_ptr<WebRtcTransport> t;
    std::shared_ptr<WebRtcIngest> ing;
    IngestInfo info;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            t = it->second;
            sessions_.erase(it);
        }
        auto ii = ingests_.find(sessionId);
        if (ii != ingests_.end()) {
            ing = ii->second;
            info = ingestInfo_[sessionId];
            ingestInfo_.erase(sessionId);
            ingests_.erase(ii);
        }
    }
    if (t) {
        t->stop();
        LOG_INFO("WebRtcManager: WHEP 会话 %s 已由客户端结束", sessionId.c_str());
    }
    if (ing) {
        endIngest(ing, info);
        LOG_INFO("WebRtcManager: WHIP 会话 %s 已由客户端结束", sessionId.c_str());
    }
    return t || ing;
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
