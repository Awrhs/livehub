#pragma once

#include "core/NonCopyable.hpp"
#include "core/Timestamp.hpp"
#include "net/Channel.hpp"
#include "net/Timer.hpp"

#include <functional>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

namespace streaming {

class EventLoop;

/**
 * TimerQueue: 基于 timerfd 的定时器队列。
 *
 * 设计要点：
 * - 用 timerfd_create(CLOCK_MONOTONIC) 把定时功能集成进 epoll
 * - 内部用 std::set 按 (到期时间, 序列号) 排序，最早到期的在前
 * - 同一时刻可有多个定时器，序列号保证顺序唯一
 * - addTimer/cancel 可跨线程调用（通过 runInLoop 切回 loop 线程）
 *
 * 简历亮点：timerfd + 有序集合，Reactor 定时任务，跨线程取消
 */
class TimerQueue : NonCopyable {
public:
    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    /// 添加定时器
    /// @param when 首次触发时刻
    /// @param interval 重复间隔（秒），0 表示一次性
    TimerId addTimer(Timer::Callback cb, Timestamp when, double interval = 0.0);

    /// 取消定时器
    void cancel(TimerId id);

private:
    using Entry = std::pair<Timestamp, Timer*>;  // (到期时间, 定时器)

    void addTimerInLoop(Timer* timer);
    void cancelInLoop(TimerId id);

    /// timerfd 可读回调
    void handleRead();

    /// 取出所有已到期定时器，返回它们（并从 set 移除）
    std::vector<Entry> getExpired(Timestamp now);

    /// 对重复的到期定时器 reset，并重新插入；重置 timerfd
    void reset(const std::vector<Entry>& expired, Timestamp now);

    /// 插入一个 timer，返回是否它是最早到期的（需要更新 timerfd）
    bool insert(Timer* timer);

    /// 调整 timerfd 的下次到期时间
    void resetTimerfd(Timestamp expiration);

    static int createTimerfd();

    EventLoop* loop_;
    int timerfd_;
    Channel timerfdChannel_;

    // 按到期时间排序的定时器集合
    std::set<Entry> timers_;

    // 待取消的定时器
    std::unordered_set<Timer*> cancellingTimers_;
};

}  // namespace streaming
