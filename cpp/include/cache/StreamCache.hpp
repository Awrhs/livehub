#pragma once

#include "core/NonCopyable.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace streaming {

class Buffer;

/**
 * StreamCache: 热点分片的内存缓存（按路径缓存字节内容）。
 *
 * 命中时可直接 append 到输出 Buffer，避免磁盘 IO。
 * 内部按字节容量做 LRU 淘汰（复用 LruCache）。
 *
 * 可选 L2（Redis，sc:<key>，TTL 120s）：配置 STREAM_REDIS_HOST 后启用。
 * - put：本地写 + 异步写透 Redis（fire-and-forget，不阻塞 Reactor）。
 * - get：本地未命中时同步查 Redis（LAN RTT <1ms，超时 50ms 兜底），
 *   命中回灌本地——本地 LRU 淘汰或节点重启后仍可从 L2 快速恢复，
 *   免去磁盘读。Redis 未配置/不可用时行为与纯本地版完全一致。
 */
class StreamCache : NonCopyable {
public:
    explicit StreamCache(size_t maxBytes = 256 * 1024 * 1024);
    ~StreamCache();

    /// 缓存一段数据的拷贝（启用 Redis 时异步写透 L2）
    void put(const std::string& key, const char* data, size_t len);

    /// 获取缓存内容；未命中返回 nullptr。返回的指针仅在本次调用期间有效。
    /// 启用 Redis 且本地未命中时查 L2 并回灌。
    std::shared_ptr<std::string> get(const std::string& key);

    void erase(const std::string& key);
    void clear();

    size_t sizeBytes() const;
    size_t sizeEntries() const;
    double hitRate() const;

private:
    static std::string l2Key(const std::string& key) { return "sc:" + key; }
    /// L2 分片保留时长（约 2 个 HLS 窗口）
    static constexpr int kL2TtlSec = 120;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace streaming
