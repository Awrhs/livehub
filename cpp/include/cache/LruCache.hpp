#pragma once

#include "core/NonCopyable.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace streaming {

/**
 * LruCache: 手写 LRU（最近最少使用）缓存。
 *
 * 用 std::list 维护访问顺序（front=最久未用，back=最近使用），
 * 用 unordered_map 做 O(1) 查找。容量同时受条目数与字节数限制。
 *
 * 简历亮点：手写 LRU、双向链表 + 哈希表 O(1)
 */
template <typename K, typename V>
class LruCache : NonCopyable {
public:
    /// @param maxEntries 最大条目数（0=不限）
    /// @param maxBytes   最大字节数（0=不限），由 valueSize 估算
    using ValueSizeFunc = std::function<size_t(const V&)>;

    explicit LruCache(size_t maxEntries = 1024,
                      size_t maxBytes = 0,
                      ValueSizeFunc valueSize = nullptr)
        : maxEntries_(maxEntries),
          maxBytes_(maxBytes),
          valueSize_(std::move(valueSize)),
          bytes_(0),
          hits_(0),
          misses_(0) {}

    std::shared_ptr<V> get(const K& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = index_.find(key);
        if (it == index_.end()) {
            ++misses_;
            return nullptr;
        }
        // 移到链表尾部（最近使用）
        entries_.splice(entries_.end(), entries_, it->second);
        ++hits_;
        return it->second->second;
    }

    void put(const K& key, V value) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = index_.find(key);
        auto valPtr = std::make_shared<V>(std::move(value));
        if (it != index_.end()) {
            // 已存在：更新
            bytes_ -= entrySize(it->second->second);
            it->second->second = valPtr;
            bytes_ += entrySize(valPtr);
            entries_.splice(entries_.end(), entries_, it->second);
        } else {
            entries_.emplace_back(key, valPtr);
            auto listIt = std::prev(entries_.end());
            index_[key] = listIt;
            bytes_ += entrySize(valPtr);
        }
        pruneLocked();
    }

    bool erase(const K& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        bytes_ -= entrySize(it->second->second);
        entries_.erase(it->second);
        index_.erase(it);
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return index_.size();
    }
    size_t bytes() const {
        std::lock_guard<std::mutex> lk(mu_);
        return bytes_;
    }
    double hitRate() const {
        size_t h, m;
        {
            std::lock_guard<std::mutex> lk(mu_);
            h = hits_; m = misses_;
        }
        size_t total = h + m;
        return total == 0 ? 0.0 : static_cast<double>(h) / total;
    }
    size_t hits() const {
        std::lock_guard<std::mutex> lk(mu_);
        return hits_;
    }
    size_t misses() const {
        std::lock_guard<std::mutex> lk(mu_);
        return misses_;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.clear();
        index_.clear();
        bytes_ = 0;
    }

private:
    using Entry = std::pair<K, std::shared_ptr<V>>;
    using ListIter = typename std::list<Entry>::iterator;

    size_t entrySize(const std::shared_ptr<V>& v) const {
        if (valueSize_ && v) return valueSize_(*v);
        return sizeof(V);
    }

    void pruneLocked() {
        while ((maxEntries_ > 0 && index_.size() > maxEntries_) ||
               (maxBytes_ > 0 && bytes_ > maxBytes_)) {
            if (entries_.empty()) break;
            auto& front = entries_.front();
            bytes_ -= entrySize(front.second);
            index_.erase(front.first);
            entries_.pop_front();
        }
    }

    mutable std::mutex mu_;
    std::list<Entry> entries_;
    std::unordered_map<K, ListIter> index_;
    size_t maxEntries_;
    size_t maxBytes_;
    ValueSizeFunc valueSize_;
    size_t bytes_;
    size_t hits_;
    size_t misses_;
};

/**
 * ShardedLruCache: 分片 LRU，降低锁竞争。
 * 按 key 的哈希分到 N 个桶，每桶独立加锁。
 */
template <typename K, typename V, size_t N = 16>
class ShardedLruCache : NonCopyable {
public:
    using ValueSizeFunc = typename LruCache<K, V>::ValueSizeFunc;

    explicit ShardedLruCache(size_t maxEntriesPerShard = 1024,
                             size_t maxBytes = 0,
                             ValueSizeFunc valueSize = nullptr) {
        size_t perShardEntries = maxEntriesPerShard;
        size_t perShardBytes = maxBytes == 0 ? 0 : maxBytes / N;
        for (size_t i = 0; i < N; ++i) {
            shards_[i] = std::make_unique<LruCache<K, V>>(
                perShardEntries, perShardBytes, valueSize);
        }
    }

    std::shared_ptr<V> get(const K& key) {
        return shard(key).get(key);
    }
    void put(const K& key, V value) {
        shard(key).put(key, std::move(value));
    }
    bool erase(const K& key) { return shard(key).erase(key); }

    size_t size() const {
        size_t total = 0;
        for (auto& s : shards_) total += s->size();
        return total;
    }
    double hitRate() const {
        size_t h = 0, m = 0;
        for (auto& s : shards_) {
            h += s->hits();
            m += s->misses();
        }
        size_t total = h + m;
        return total == 0 ? 0.0 : static_cast<double>(h) / total;
    }

private:
    LruCache<K, V>& shard(const K& key) {
        size_t idx = std::hash<K>{}(key) % N;
        return *shards_[idx];
    }
    std::unique_ptr<LruCache<K, V>> shards_[N];
};

}  // namespace streaming
