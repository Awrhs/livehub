#pragma once

#include "core/NonCopyable.hpp"
#include "core/Timestamp.hpp"

#include <atomic>
#include <functional>

namespace streaming {

/**
 * TimerId: 定时器的唯一句柄，用于取消。
 */
struct TimerId {
    TimerId() : timer(nullptr), sequence(0) {}
    TimerId(class Timer* t, int64_t seq) : timer(t), sequence(seq) {}

    Timer* timer;
    int64_t sequence;

    explicit operator bool() const { return timer != nullptr; }
};

/**
 * Timer: 一个定时任务。
 *
 * 持有回调、首次触发时刻、重复间隔与序列号。
 * 不拥有线程语义，由 TimerQueue 在 EventLoop 线程内调度。
 */
class Timer : NonCopyable {
public:
    using Callback = std::function<void()>;

    Timer(Callback cb, Timestamp when, double intervalSeconds)
        : callback_(std::move(cb)),
          expiration_(when),
          interval_(intervalSeconds),
          repeat_(intervalSeconds > 0.0),
          sequence_(numCreated_.fetch_add(1) + 1) {}

    void run() const {
        if (callback_) callback_();
    }

    Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    int64_t sequence() const { return sequence_; }

    /// 重复定时器：以 now 为基准重新计算下一次触发
    void restart(Timestamp now) {
        if (repeat_) {
            expiration_ = addTime(now, interval_);
        } else {
            expiration_ = Timestamp();  // 一次性定时器置无效
        }
    }

private:
    const Callback callback_;
    Timestamp expiration_;
    const double interval_;     // 秒，0 表示一次性
    const bool repeat_;
    const int64_t sequence_;

    static std::atomic<int64_t> numCreated_;
};

}  // namespace streaming
