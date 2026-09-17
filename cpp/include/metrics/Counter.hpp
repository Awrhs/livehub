#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace streaming {

/**
 * Counter: 单调递增计数器（无锁原子）。
 */
class Counter {
public:
    Counter() = default;
    explicit Counter(std::string name, std::string help = "")
        : name_(std::move(name)), help_(std::move(help)) {}

    void inc(uint64_t v = 1) { value_.fetch_add(v, std::memory_order_relaxed); }
    uint64_t value() const { return value_.load(std::memory_order_relaxed); }
    void reset() { value_.store(0, std::memory_order_relaxed); }

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<uint64_t> value_{0};
};

}  // namespace streaming
