#pragma once

#include "core/NonCopyable.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace streaming {

class EventLoop;

/**
 * TokenBlacklist: 已注销 JWT（按 jti）黑名单。
 *
 * 双层存储：
 * - 本地内存表（必写）：本节点注销的 jti 立即生效；Redis 不可用时的
 *   降级路径；本地 sweep 清理过期项。
 * - Redis（可选，bl:<jti>，TTL = token 剩余有效期）：跨节点共享。
 *   contains() 本地未命中时查 Redis，命中则回灌本地（带原过期时间），
 *   使其他节点的注销在本节点同样生效。
 *
 * 查询顺序本地优先：未注销 token 常见路径下两层都 miss，本地命中
 * （本节点已注销）时免去 Redis 往返。
 */
class TokenBlacklist : NonCopyable {
public:
    TokenBlacklist() = default;

    /// 注销：写本地 + 尽力写 Redis（TTL = expireAtSec - now）
    void add(const std::string& jti, int64_t expireAtSec);

    /// 是否已注销（本地 → Redis，Redis 命中回灌本地）
    bool contains(const std::string& jti) const;

    /// 本地表条目数
    size_t size() const;

    /// 清理本地表中 expireAtSec <= nowSec 的项
    size_t sweep(int64_t nowSec);

private:
    struct Entry {
        int64_t expireAtSec;
    };
    mutable std::mutex mu_;
    mutable std::unordered_map<std::string, Entry> entries_;  // contains() 会回灌 Redis 命中
};

}  // namespace streaming
