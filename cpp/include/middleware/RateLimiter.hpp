#pragma once

#include "core/NonCopyable.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace streaming {

/**
 * TokenBucket: 单 key 的令牌桶。
 *
 * 令牌以 rate 每秒补充，容量 burst，取令牌时按时间差补充。
 */
class TokenBucket {
public:
    TokenBucket() = default;
    TokenBucket(double rate, double burst)
        : rate_(rate), burst_(burst), tokens_(burst) {
        last_ = std::chrono::steady_clock::now();
    }

    bool tryConsume(double cost = 1.0) {
        auto now = std::chrono::steady_clock::now();
        double elapsed =
            std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(burst_, tokens_ + elapsed * rate_);
        if (tokens_ >= cost) {
            tokens_ -= cost;
            return true;
        }
        return false;
    }

private:
    double rate_ = 0;
    double burst_ = 0;
    double tokens_ = 0;
    std::chrono::steady_clock::time_point last_;
};

/**
 * RatePolicy: 路由级限流策略。
 *
 * - pathPrefix：按请求路径做最长前缀匹配（"/api/token" 优先于 "/api/"）。
 * - name：策略名；同名的多个前缀（如 /api/token 与 /api/logout）共享
 *   同一配额桶（key = "ip name"）。
 * - limitPerMin：每分钟配额；<=0 表示该路由不限流。
 */
struct RatePolicy {
    std::string name;
    std::string pathPrefix;
    int limitPerMin = 0;
};

/**
 * RateLimiter: 按 key（IP+策略）限流。
 *
 * - 分片锁（N=16）降低并发竞争。
 * - 定时器周期清理空闲桶，避免内存无限增长。
 * - 白名单 IP（内网）跳过限流。
 * - 路由策略表：addPolicy 注册（服务启动阶段），matchPolicy 按最长
 *   前缀匹配。策略注册后不应再修改（matchPolicy 返回内部指针）。
 */
class RateLimiter : NonCopyable {
public:
    explicit RateLimiter(double defaultRate = 100, double defaultBurst = 200);

    /// 尝试通过；返回 true 放行，false 限流
    bool allow(const std::string& key);

    /// 按指定速率消费：桶不存在时以该速率创建（策略桶用），
    /// 白名单同样生效
    bool allow(const std::string& key, double rate, double burst);

    /// 为指定 key 设置独立速率
    void setRate(const std::string& key, double rate, double burst);

    /// 注册路由策略（可多次调用；服务启动阶段完成）
    void addPolicy(std::string name, std::string pathPrefix,
                   int limitPerMin);

    /// 最长前缀匹配；无命中返回 nullptr
    const RatePolicy* matchPolicy(const std::string& path) const;

    /// 加入白名单（如内网 IP）
    void addWhitelist(const std::string& ip) { whitelist_.insert(ip); }
    bool isWhitelisted(const std::string& ip) const {
        return whitelist_.count(ip) > 0;
    }

    /// 清理超过 idleSec 未访问的桶
    void sweep(int idleSec = 300);

    size_t size() const;

private:
    struct Entry {
        TokenBucket bucket;
        std::chrono::steady_clock::time_point lastAccess;
    };

    static constexpr size_t kShards = 16;
    struct Shard {
        mutable std::mutex mu;
        std::unordered_map<std::string, Entry> buckets;
    };

    size_t shardIndex(const std::string& key) const;

    double defaultRate_;
    double defaultBurst_;
    std::vector<Shard> shards_{kShards};
    std::unordered_set<std::string> whitelist_;
    mutable std::mutex wlMu_;
    std::vector<RatePolicy> policies_;
    mutable std::mutex policyMu_;
};

}  // namespace streaming
