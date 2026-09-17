#include "middleware/RateLimiter.hpp"

#include <functional>

namespace streaming {

RateLimiter::RateLimiter(double defaultRate, double defaultBurst)
    : defaultRate_(defaultRate), defaultBurst_(defaultBurst) {}

size_t RateLimiter::shardIndex(const std::string& key) const {
    return std::hash<std::string>{}(key) % kShards;
}

bool RateLimiter::allow(const std::string& key) {
    return allow(key, defaultRate_, defaultBurst_);
}

bool RateLimiter::allow(const std::string& key, double rate, double burst) {
    // 提取 IP（key 形如 "IP policyName"）做白名单判断
    auto sp = key.find(' ');
    std::string ip = sp == std::string::npos ? key : key.substr(0, sp);
    {
        std::lock_guard<std::mutex> lk(wlMu_);
        if (whitelist_.count(ip) > 0) return true;
    }

    size_t idx = shardIndex(key);
    auto& shard = shards_[idx];
    std::lock_guard<std::mutex> lk(shard.mu);
    auto it = shard.buckets.find(key);
    if (it == shard.buckets.end()) {
        // 桶不存在时以调用方给定的速率创建（策略桶各用各的配额）
        it = shard.buckets
                 .emplace(key,
                          Entry{TokenBucket(rate, burst),
                                std::chrono::steady_clock::now()})
                 .first;
    }
    it->second.lastAccess = std::chrono::steady_clock::now();
    return it->second.bucket.tryConsume(1.0);
}

void RateLimiter::setRate(const std::string& key, double rate, double burst) {
    size_t idx = shardIndex(key);
    auto& shard = shards_[idx];
    std::lock_guard<std::mutex> lk(shard.mu);
    shard.buckets[key] = Entry{TokenBucket(rate, burst),
                                std::chrono::steady_clock::now()};
}

void RateLimiter::addPolicy(std::string name, std::string pathPrefix,
                            int limitPerMin) {
    std::lock_guard<std::mutex> lk(policyMu_);
    policies_.push_back(
        RatePolicy{std::move(name), std::move(pathPrefix), limitPerMin});
}

const RatePolicy* RateLimiter::matchPolicy(
    const std::string& path) const {
    std::lock_guard<std::mutex> lk(policyMu_);
    const RatePolicy* best = nullptr;
    for (const auto& p : policies_) {
        if (path.compare(0, p.pathPrefix.size(), p.pathPrefix) == 0) {
            if (!best || p.pathPrefix.size() > best->pathPrefix.size()) {
                best = &p;
            }
        }
    }
    return best;
}

void RateLimiter::sweep(int idleSec) {
    auto now = std::chrono::steady_clock::now();
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lk(shard.mu);
        for (auto it = shard.buckets.begin();
             it != shard.buckets.end();) {
            double idle =
                std::chrono::duration<double>(now - it->second.lastAccess)
                    .count();
            if (idle > idleSec) {
                it = shard.buckets.erase(it);
            } else {
                ++it;
            }
        }
    }
}

size_t RateLimiter::size() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lk(shard.mu);
        total += shard.buckets.size();
    }
    return total;
}

}  // namespace streaming
