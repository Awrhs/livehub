#include "cache/StreamCache.hpp"
#include "cache/LruCache.hpp"
#include "cache/RedisClient.hpp"

namespace streaming {

struct StreamCache::Impl {
    explicit Impl(size_t maxBytes)
        : cache(4096, maxBytes,
                [](const std::string& v) { return v.size(); }) {}
    LruCache<std::string, std::string> cache;
};

StreamCache::StreamCache(size_t maxBytes)
    : impl_(new Impl(maxBytes)) {}

StreamCache::~StreamCache() = default;

void StreamCache::put(const std::string& key, const char* data, size_t len) {
    impl_->cache.put(key, std::string(data, len));
    // L2 写透：异步 fire-and-forget，绝不阻塞 Reactor（队列积压时自动丢弃）
    auto& redis = RedisClient::instance();
    if (redis.enabled()) {
        redis.setWithTtlAsync(l2Key(key), std::string(data, len),
                              kL2TtlSec);
    }
}

std::shared_ptr<std::string> StreamCache::get(const std::string& key) {
    auto local = impl_->cache.get(key);
    if (local) return local;
    // 本地未命中 → 查 L2（本地 LRU 淘汰/节点重启后的快速恢复路径）。
    // 同步等待有 50ms 超时上限：此路径本来就要读磁盘，开销可接受。
    auto& redis = RedisClient::instance();
    if (!redis.enabled()) return nullptr;
    std::string val;
    if (!redis.get(l2Key(key), &val)) return nullptr;
    auto hit = std::make_shared<std::string>(std::move(val));
    impl_->cache.put(key, *hit);
    return hit;
}

void StreamCache::erase(const std::string& key) {
    impl_->cache.erase(key);
}

void StreamCache::clear() {
    impl_->cache.clear();
}

size_t StreamCache::sizeBytes() const {
    return impl_->cache.bytes();
}

size_t StreamCache::sizeEntries() const {
    return impl_->cache.size();
}

double StreamCache::hitRate() const {
    return impl_->cache.hitRate();
}

}  // namespace streaming
