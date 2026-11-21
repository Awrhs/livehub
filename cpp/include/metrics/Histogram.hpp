#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

/**
 * Histogram: Prometheus 风格直方图（固定桶 + count/sum）。
 *
 * 线程安全：桶计数用原子，sum 用 double 受锁保护（简化）。
 * 用于 HTTP 延迟分位统计。
 */
class Histogram {
public:
    Histogram() = default;
    Histogram(std::string name, std::string help,
              std::vector<double> buckets = {0.005, 0.01, 0.025, 0.05, 0.1,
                                              0.25, 0.5, 1.0, 2.5, 5.0, 10.0})
        : name_(std::move(name)),
          help_(std::move(help)),
          buckets_(std::move(buckets)),
          counts_(buckets_.size() + 1) {}  // 值初始化为 0

    void observe(double value) {
        for (size_t i = 0; i < buckets_.size(); ++i) {
            if (value <= buckets_[i]) {
                counts_[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        if (value > buckets_.back()) {
            counts_[buckets_.size()].fetch_add(1,
                                                std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lk(sumMu_);
            sum_ += value;
        }
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t count() const {
        return count_.load(std::memory_order_relaxed);
    }
    double sum() const {
        std::lock_guard<std::mutex> lk(sumMu_);
        return sum_;
    }

    const std::vector<double>& buckets() const { return buckets_; }
    uint64_t bucketCount(size_t i) const {
        return counts_[i].load(std::memory_order_relaxed);
    }

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }

private:
    std::string name_;
    std::string help_;
    std::vector<double> buckets_;
    std::vector<std::atomic<uint64_t>> counts_;
    std::atomic<uint64_t> count_{0};
    mutable std::mutex sumMu_;
    double sum_ = 0.0;
};

}  // namespace streaming
