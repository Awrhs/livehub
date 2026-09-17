#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace streaming {

/**
 * Gauge: 可增可减的瞬时值。
 */
class Gauge {
public:
    Gauge() = default;
    explicit Gauge(std::string name, std::string help = "")
        : name_(std::move(name)), help_(std::move(help)) {}

    void set(int64_t v) { value_.store(v, std::memory_order_relaxed); }
    void inc(int64_t v = 1) { value_.fetch_add(v, std::memory_order_relaxed); }
    void dec(int64_t v = 1) { value_.fetch_sub(v, std::memory_order_relaxed); }
    int64_t value() const { return value_.load(std::memory_order_relaxed); }

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<int64_t> value_{0};
};

}  // namespace streaming
