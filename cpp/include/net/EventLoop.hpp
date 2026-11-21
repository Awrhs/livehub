#pragma once

#include "core/NonCopyable.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "net/EpollPoller.hpp"
#include "net/Timer.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/eventfd.h>
#include <thread>
#include <vector>

namespace streaming {

class Channel;
class TimerQueue;
struct TimerId;

/**
 * EventLoop: 事件循环，Reactor 模式的核心。
 *
 * 设计要点：
 * - 每个 EventLoop 绑定一个线程（one loop per thread）
 * - 持有 EpollPoller，循环 epoll_wait 并分发事件
 * - 跨线程任务：runInLoop() 把回调加入 pendingFunctors，
 *   通过 eventfd 唤醒本 loop 所在线程执行
 * - 保证回调在所属线程执行，避免数据竞争
 *
 * 简历亮点：事件驱动核心，理解 eventfd 唤醒机制、跨线程调用
 */
class EventLoop : NonCopyable {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    /// 循环（在所属线程调用）
    void loop();

    /// 退出循环（可跨线程调用）
    void quit();

    /// 在本 loop 线程执行 cb；若已在 loop 线程则直接执行
    void runInLoop(Functor cb);

    /// 把 cb 加入队列，下一轮 loop 执行
    void queueInLoop(Functor cb);

    /// 更新 Channel（在 loop 线程执行 epoll_ctl）
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);

    /// 断言当前是否在 loop 线程
    bool isInLoopThread() const {
        return threadId_ == std::this_thread::get_id();
    }
    void assertInLoopThread() {
        if (!isInLoopThread()) {
            LOG_FATAL("assertInLoopThread failed: not in loop thread");
        }
    }

    /// 获取本 loop 所属线程
    std::thread::id threadId() const { return threadId_; }

    // ---- 定时器 ----
    /// delaySeconds 后执行一次
    TimerId runAfter(double delaySeconds, std::function<void()> cb);
    /// 每 intervalSeconds 执行一次
    TimerId runEvery(double intervalSeconds, std::function<void()> cb);
    /// 取消定时器
    void cancel(TimerId id);

    /// 上一轮事件处理耗时（秒）
    double eventHandlingTime() const { return eventHandlingTime_; }

private:
    void handleRead();         // eventfd 可读回调
    void doPendingFunctors();  // 执行跨线程任务

    using ChannelList = std::vector<Channel*>;

    std::atomic<bool> looping_;
    std::atomic<bool> quit_;
    const std::thread::id threadId_;

    std::unique_ptr<EpollPoller> poller_;
    std::unique_ptr<TimerQueue> timerQueue_;
    int wakeupFd_;                          // eventfd
    std::unique_ptr<Channel> wakeupChannel_;

    ChannelList activeChannels_;
    std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;  // 跨线程任务队列
    std::atomic<bool> callingPending_;      // 正在执行 pending
    double eventHandlingTime_ = 0.0;        // 上一轮事件处理耗时（秒）
};

}  // namespace streaming
