#pragma once

#include "core/NonCopyable.hpp"
#include "core/Logger.hpp"

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace streaming {

/**
 * MemoryPool: 定长块内存池。
 *
 * 设计要点：
 * - 预分配一大块内存，按固定 BlockSize 切分
 * - 分配/回收 O(1)：空闲链表
 * - 避免频繁 malloc/free 造成的碎片与系统调用开销
 * - 线程安全（加锁）；高频场景可改为 per-thread 池或无锁实现
 *
 * 简历亮点：手写内存池，理解内存碎片、空闲链表、分配器原理
 */
class MemoryPool : NonCopyable {
public:
    /**
     * @param blockSize 每个块的大小（字节）
     * @param blockCount 初始块数
     */
    MemoryPool(size_t blockSize, size_t blockCount)
        : blockSize_(blockSize),
          chunk_(nullptr),
          chunkSize_(blockSize * blockCount) {
        // 用 posix_memalign 分配对齐的大块
        void* mem = nullptr;
        if (::posix_memalign(&mem, 64, chunkSize_) != 0 || !mem) {
            LOG_FATAL("MemoryPool posix_memalign failed");
        }
        chunk_ = static_cast<char*>(mem);

        // 将所有块串成空闲链表
        freeList_.reserve(blockCount);
        for (size_t i = 0; i < blockCount; ++i) {
            char* p = chunk_ + i * blockSize_;
            freeList_.push_back(p);
        }

        LOG_INFO("MemoryPool created: %zu blocks x %zu bytes = %zu bytes",
                 blockCount, blockSize_, chunkSize_);
    }

    ~MemoryPool() {
        if (chunk_) {
            ::free(chunk_);
        }
    }

    /// 分配一个块，返回 nullptr 表示池已空
    void* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (freeList_.empty()) {
            LOG_WARN("MemoryPool exhausted (blockSize=%zu)", blockSize_);
            return nullptr;
        }
        char* p = freeList_.back();
        freeList_.pop_back();
        return p;
    }

    /// 归还一个块（p 必须来自本池）
    void deallocate(void* p) {
        if (!p) return;
        std::lock_guard<std::mutex> lock(mutex_);
        freeList_.push_back(static_cast<char*>(p));
    }

    size_t blockSize() const { return blockSize_; }
    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return freeList_.size();
    }

private:
    size_t blockSize_;
    char* chunk_;        // 大块内存起点
    size_t chunkSize_;
    std::vector<char*> freeList_;  // 空闲链表
    mutable std::mutex mutex_;
};

/**
 * RAII 包装：分配后自动归还
 */
class PoolAllocation {
public:
    PoolAllocation(MemoryPool& pool)
        : pool_(&pool), ptr_(pool_.allocate()) {}

    ~PoolAllocation() {
        if (ptr_) pool_.deallocate(ptr_);
    }

    PoolAllocation(const PoolAllocation&) = delete;
    PoolAllocation& operator=(const PoolAllocation&) = delete;

    PoolAllocation(PoolAllocation&& other) noexcept
        : pool_(other.pool_), ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    void* get() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    MemoryPool* pool_;
    void* ptr_;
};

}  // namespace streaming
