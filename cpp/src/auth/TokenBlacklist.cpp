#include "auth/TokenBlacklist.hpp"

#include "cache/RedisClient.hpp"
#include "core/Timestamp.hpp"

namespace streaming {

namespace {
std::string redisKey(const std::string& jti) { return "bl:" + jti; }
}  // namespace

void TokenBlacklist::add(const std::string& jti, int64_t expireAtSec) {
    // 1) 本地必写：本节点立即生效
    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_[jti] = Entry{expireAtSec};
    }
    // 2) Redis 尽力写（跨节点共享）；值 = 过期时间戳，回灌时恢复 TTL 语义
    int64_t now = Timestamp::now().secondsSinceEpoch();
    int64_t ttl = expireAtSec - now;
    if (ttl > 0) {
        RedisClient::instance().setWithTtl(
            redisKey(jti), std::to_string(expireAtSec),
            static_cast<int>(ttl));
    }
}

bool TokenBlacklist::contains(const std::string& jti) const {
    // 本地命中：最快路径（含本节点注销 + Redis 回灌过的项）
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(jti);
        if (it != entries_.end() && it->second.expireAtSec >
                                         Timestamp::now()
                                             .secondsSinceEpoch()) {
            return true;
        }
    }
    // 本地未命中 → 查 Redis（其他节点注销的 jti）；命中则回灌本地
    std::string val;
    if (!RedisClient::instance().get(redisKey(jti), &val)) return false;
    int64_t expireAtSec = 0;
    try {
        expireAtSec = std::stoll(val);
    } catch (...) {
        return false;  // 值异常（被外部写坏）按未注销处理
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_[jti] = Entry{expireAtSec};
    }
    return true;
}

size_t TokenBlacklist::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

size_t TokenBlacklist::sweep(int64_t nowSec) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expireAtSec <= nowSec) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

}  // namespace streaming
