#include "middleware/RateLimiter.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace streaming;

TEST(token_bucket_burst) {
    TokenBucket tb(10.0, 5.0);
    // 初始 5 个令牌，可连续消费 5 次
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(tb.tryConsume(1.0));
    }
    // 第 6 次被拒
    ASSERT_FALSE(tb.tryConsume(1.0));
}

TEST(token_bucket_refill) {
    TokenBucket tb(100.0, 2.0);
    ASSERT_TRUE(tb.tryConsume(2.0));  // 耗尽
    ASSERT_FALSE(tb.tryConsume(1.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // 100/s * 0.05s = 5 个令牌，应可通过
    ASSERT_TRUE(tb.tryConsume(1.0));
}

TEST(rate_limiter_independent_keys) {
    RateLimiter rl(10.0, 10.0);
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(rl.allow("ip-a"));
    }
    ASSERT_FALSE(rl.allow("ip-a"));
    // 另一个 key 独立计数，仍可通过
    ASSERT_TRUE(rl.allow("ip-b"));
}

TEST(rate_limiter_whitelist) {
    RateLimiter rl(1.0, 1.0);
    rl.addWhitelist("10.0.0.1");
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(rl.allow("10.0.0.1"));
    }
}

TEST(rate_limiter_sweep) {
    RateLimiter rl(10.0, 10.0);
    rl.allow("k1");
    rl.allow("k2");
    ASSERT_GT(rl.size(), size_t(0));
    // 立即 sweep，idleSec=0 应清理所有
    rl.sweep(0);
    ASSERT_EQ(rl.size(), size_t(0));
}

// ---- 分路由策略（最长前缀匹配）----

TEST(rate_policy_longest_prefix_match) {
    RateLimiter rl(100.0, 200.0);
    rl.addPolicy("api", "/api/", 100);
    rl.addPolicy("auth", "/api/token", 10);
    rl.addPolicy("playback", "/live/", 300);

    // /api/token 命中更长的 auth 前缀，而非 /api/
    const RatePolicy* p = rl.matchPolicy("/api/token");
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(p->name, std::string("auth"));
    ASSERT_EQ(p->limitPerMin, 10);

    // 同前缀族其他路径命中 api
    p = rl.matchPolicy("/api/health");
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(p->name, std::string("api"));

    // /live/ 命中 playback
    p = rl.matchPolicy("/live/test.m3u8");
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(p->name, std::string("playback"));

    // 完全不匹配 → nullptr（走全局默认）
    ASSERT_TRUE(rl.matchPolicy("/metrics") == nullptr);
    ASSERT_TRUE(rl.matchPolicy("/") == nullptr);
    ASSERT_TRUE(rl.matchPolicy("/api") == nullptr);  // 无尾斜杠不匹配 /api/
}

TEST(rate_policy_custom_rate_burst) {
    RateLimiter rl(1000.0, 1000.0);  // 全局默认很宽松
    // 策略桶：2/min → burst 2
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(rl.allow("1.2.3.4 auth", 2.0 / 60.0, 2.0));
    }
    ASSERT_FALSE(rl.allow("1.2.3.4 auth", 2.0 / 60.0, 2.0));
    // 同 IP 另一策略独立计数
    ASSERT_TRUE(rl.allow("1.2.3.4 api", 100.0 / 60.0, 100.0));
    // 另一 IP 同策略独立计数
    ASSERT_TRUE(rl.allow("5.6.7.8 auth", 2.0 / 60.0, 2.0));
}

TEST(rate_policy_disabled) {
    // limitPerMin<=0 由中间件直接放行（不走桶）；此处验证策略表本身
    RateLimiter rl(100.0, 200.0);
    rl.addPolicy("free", "/free/", 0);
    const RatePolicy* p = rl.matchPolicy("/free/anything");
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(p->limitPerMin, 0);
}
