#pragma once

#include "core/NonCopyable.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace streaming {

/**
 * RingBuffer: 无锁单生产者单消费者（SPSC）环形缓冲。
 *
 * head_ 由生产者写、消费者读；tail_ 由消费者写、生产者读。
 * 用 alignas(64) 避免 false sharing，acquire/release 保证可见性。
 * 容量向上取整为 2 的幂，用掩码做 wrap-around。
 *
 * 典型用途：RTMP 解复用线程（生产者）→ HLS 切片线程（消费者）。
 *
 * 简历亮点：无锁编程、内存序、cache line 对齐、SPSC 队列
 */
class RingBuffer : NonCopyable {
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(roundUpPow2(capacity)),
          mask_(capacity_ - 1),
          buffer_(new char[capacity_]) {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    /// 可写字节数
    size_t writableBytes() const {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        return capacity_ - (head - tail);
    }
    /// 可读字节数
    size_t readableBytes() const {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        return head - tail;
    }

    /// 写入数据，返回实际写入字节数（空间不足时部分写入）
    size_t write(const void* data, size_t len) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t freeBytes = capacity_ - (head - tail);
        if (len > freeBytes) len = freeBytes;

        size_t offset = head & mask_;
        size_t toEnd = capacity_ - offset;
        size_t first = len < toEnd ? len : toEnd;
        std::memcpy(buffer_.get() + offset, data, first);
        if (len > first) {
            std::memcpy(buffer_.get(),
                        static_cast<const char*>(data) + first,
                        len - first);
        }
        head_.store(head + len, std::memory_order_release);
        return len;
    }

    /// 读出数据，返回实际读出字节数
    size_t read(void* out, size_t len) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        size_t avail = head - tail;
        if (len > avail) len = avail;

        size_t offset = tail & mask_;
        size_t toEnd = capacity_ - offset;
        size_t first = len < toEnd ? len : toEnd;
        std::memcpy(out, buffer_.get() + offset, first);
        if (len > first) {
            std::memcpy(static_cast<char*>(out) + first, buffer_.get(),
                        len - first);
        }
        tail_.store(tail + len, std::memory_order_release);
        return len;
    }

    size_t capacity() const { return capacity_; }

private:
    static size_t roundUpPow2(size_t v) {
        if (v < 2) return 2;
        v--;
        for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) {
            v |= v >> shift;
        }
        return v + 1;
    }

    alignas(64) std::atomic<size_t> head_;  // 生产者
    alignas(64) std::atomic<size_t> tail_;  // 消费者
    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<char[]> buffer_;
};

}  // namespace streaming
