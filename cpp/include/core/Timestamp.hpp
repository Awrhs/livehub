#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace streaming {

/**
 * Timestamp: 微秒级时间戳工具。
 * 封装自 epoch 起的微秒数，提供格式化与时差计算。
 */
class Timestamp {
public:
    Timestamp() : microSecondsSinceEpoch_(0) {}
    explicit Timestamp(int64_t microSecondsSinceEpoch)
        : microSecondsSinceEpoch_(microSecondsSinceEpoch) {}

    /// 当前时间
    static Timestamp now() {
        using namespace std::chrono;
        auto us = duration_cast<microseconds>(
            system_clock::now().time_since_epoch()).count();
        return Timestamp(static_cast<int64_t>(us));
    }

    /// 无效时间戳
    static Timestamp invalid() { return Timestamp(); }

    bool valid() const { return microSecondsSinceEpoch_ > 0; }

    int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }
    int64_t secondsSinceEpoch() const {
        return microSecondsSinceEpoch_ / kMicroSecondsPerSecond;
    }

    /// 两个时间戳的差（秒，浮点）
    double operator-(const Timestamp& rhs) const {
        return static_cast<double>(microSecondsSinceEpoch_ -
                                   rhs.microSecondsSinceEpoch_) /
               kMicroSecondsPerSecond;
    }

    /// 格式化为 YYYY-MM-DD HH:MM:SS.ffffff
    std::string toFormattedString(bool showMicroseconds = true) const;

    static const int kMicroSecondsPerSecond = 1000 * 1000;

private:
    int64_t microSecondsSinceEpoch_;
};

/// 判断是否在之后
inline bool operator<(const Timestamp& lhs, const Timestamp& rhs) {
    return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}
inline bool operator==(const Timestamp& lhs, const Timestamp& rhs) {
    return lhs.microSecondsSinceEpoch() == rhs.microSecondsSinceEpoch();
}

/// 在当前时间上增加秒数
inline Timestamp addTime(Timestamp timestamp, double seconds) {
    int64_t delta = static_cast<int64_t>(seconds *
                                         Timestamp::kMicroSecondsPerSecond);
    return Timestamp(timestamp.microSecondsSinceEpoch() + delta);
}

}  // namespace streaming
