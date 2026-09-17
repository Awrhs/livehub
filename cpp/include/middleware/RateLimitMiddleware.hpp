#pragma once

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "cache/RedisClient.hpp"
#include "core/NonCopyable.hpp"
#include "middleware/RateLimiter.hpp"
#include "net/TcpConnection.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

/**
 * RateLimitMiddleware: 把 RateLimiter 适配为 HttpServer 中间件。
 *
 * 路由策略（RateLimiter::matchPolicy 最长前缀匹配）：
 * - 命中策略且 limitPerMin>0：分布式限流（两层）：
 *     1) Redis 固定窗口 INCR（rl:<policy>:<ip>，60s 窗口，跨节点共享
 *        配额）；Redis 可用时超额即拒。
 *     2) 本地 sharded 令牌桶（速率 limitPerMin/60，突发 limitPerMin），
 *        作为 Redis 不可用/抖动时的兜底——两层都过才放行。
 * - 命中策略且 limitPerMin<=0：该路由不限流。
 * - 未命中任何策略：按 "clientIp default" 走全局默认配额（仅本地层）。
 * Redis 未配置/超时/熔断一律 fail-open：跳过分布式层，仅本地判定。
 * 超限返回 429 + Retry-After。
 */
class RateLimitMiddleware : NonCopyable {
public:
    explicit RateLimitMiddleware(std::shared_ptr<RateLimiter> limiter)
        : limiter_(std::move(limiter)) {}

    bool operator()(const std::shared_ptr<TcpConnection>&,
                    HttpRequest& req, HttpResponse& resp) {
        if (!limiter_) return true;
        const std::string& path = req.path();

        const RatePolicy* p = limiter_->matchPolicy(path);
        if (p != nullptr) {
            if (p->limitPerMin <= 0) return true;  // 该路由关闭限流

            // 1) Redis 分布式层（跨节点配额共享）
            auto& redis = RedisClient::instance();
            if (redis.enabled()) {
                int64_t count = 0;
                if (redis.incrWithTtl("rl:" + p->name + ":" + req.clientIp,
                                      60, &count)) {
                    if (count > p->limitPerMin) return reject(resp);
                    // Redis 层通过 → 继续走本地层
                }
                // Redis 失败（超时/熔断）→ fail-open，仅本地层
            }

            // 2) 本地令牌桶层
            std::string key = req.clientIp + " " + p->name;
            double rate = static_cast<double>(p->limitPerMin) / 60.0;
            double burst = static_cast<double>(p->limitPerMin);
            if (!limiter_->allow(key, rate, burst)) return reject(resp);
            return true;
        }

        // 无策略命中：全局默认（按 IP 聚合，仅本地层）
        if (!limiter_->allow(req.clientIp + " default")) return reject(resp);
        return true;
    }

private:
    static bool reject(HttpResponse& resp) {
        resp.setStatusCode(HttpStatusCode::k429TooManyRequests);
        resp.setContentType("application/json");
        resp.addHeader("Retry-After", "1");
        resp.setBody("{\"error\":\"rate limit exceeded\"}");
        return false;
    }

    std::shared_ptr<RateLimiter> limiter_;
};

}  // namespace streaming
