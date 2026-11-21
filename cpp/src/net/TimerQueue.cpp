#include "net/TimerQueue.hpp"
#include "net/EventLoop.hpp"
#include "core/Logger.hpp"

#include <sys/timerfd.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <algorithm>

namespace streaming {

std::atomic<int64_t> Timer::numCreated_(0);

int TimerQueue::createTimerfd() {
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                   TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0) {
        LOG_FATAL("timerfd_create failed: %s", strerror(errno));
    }
    return timerfd;
}

// 计算从 now 到 expiration 的时间差，转成 timespec（相对时间）
static struct timespec howMuchTimeFromNow(Timestamp expiration) {
    int64_t microseconds =
        expiration.microSecondsSinceEpoch() -
        Timestamp::now().microSecondsSinceEpoch();
    if (microseconds < 100) microseconds = 100;  // 最小 100us，避免忙等
    struct timespec ts{};
    ts.tv_sec = static_cast<time_t>(microseconds / Timestamp::kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>(
        (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
    return ts;
}

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(loop, timerfd_) {
    timerfdChannel_.setReadCallback(
        [this](Timestamp) { this->handleRead(); });
    timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue() {
    timerfdChannel_.disableAll();
    timerfdChannel_.remove();
    ::close(timerfd_);
    // 释放所有未触发的定时器
    for (const Entry& entry : timers_) {
        delete entry.second;
    }
}

TimerId TimerQueue::addTimer(Timer::Callback cb, Timestamp when,
                             double interval) {
    Timer* timer = new Timer(std::move(cb), when, interval);
    loop_->runInLoop([this, timer] { this->addTimerInLoop(timer); });
    return TimerId(timer, timer->sequence());
}

void TimerQueue::cancel(TimerId id) {
    loop_->runInLoop([this, id] { this->cancelInLoop(id); });
}

void TimerQueue::addTimerInLoop(Timer* timer) {
    loop_->assertInLoopThread();
    bool earliestChanged = insert(timer);
    if (earliestChanged) {
        resetTimerfd(timer->expiration());
    }
}

void TimerQueue::cancelInLoop(TimerId id) {
    loop_->assertInLoopThread();
    Timer* timer = id.timer;
    auto it = timers_.find(Entry(timer->expiration(), timer));
    if (it != timers_.end()) {
        timers_.erase(it);
        delete timer;
    }
}

bool TimerQueue::insert(Timer* timer) {
    bool earliestChanged = false;
    Timestamp when = timer->expiration();
    auto it = timers_.begin();
    if (it == timers_.end() || when < it->first) {
        earliestChanged = true;
    }
    timers_.insert(Entry(when, timer));
    return earliestChanged;
}

void TimerQueue::resetTimerfd(Timestamp expiration) {
    struct itimerspec newValue{};
    struct itimerspec oldValue{};
    newValue.it_value = howMuchTimeFromNow(expiration);
    int ret = ::timerfd_settime(timerfd_, 0, &newValue, &oldValue);
    if (ret < 0) {
        LOG_ERROR("timerfd_settime failed: %s", strerror(errno));
    }
}

void TimerQueue::handleRead() {
    loop_->assertInLoopThread();
    Timestamp now = Timestamp::now();

    // 读取 timerfd，清除可读事件
    uint64_t howmany = 0;
    ssize_t n = ::read(timerfd_, &howmany, sizeof(howmany));
    if (n != sizeof(howmany)) {
        LOG_ERROR("TimerQueue::handleRead reads %zd bytes", n);
    }

    std::vector<Entry> expired = getExpired(now);

    for (const Entry& entry : expired) {
        entry.second->run();
    }

    reset(expired, now);
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now) {
    std::vector<Entry> expired;
    Entry sentinel(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
    // lower_bound: 第一个不小于 sentinel 的位置，即到期时间 > now 的起点
    auto it = timers_.lower_bound(sentinel);
    // 所有到期时间 <= now 的都已到期
    std::copy(timers_.begin(), it, std::back_inserter(expired));
    timers_.erase(timers_.begin(), it);
    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now) {
    Timestamp nextExpire;
    for (const Entry& entry : expired) {
        if (entry.second->repeat()) {
            entry.second->restart(now);
            insert(entry.second);
        } else {
            delete entry.second;
        }
    }

    if (!timers_.empty()) {
        nextExpire = timers_.begin()->second->expiration();
    }
    if (nextExpire.valid()) {
        resetTimerfd(nextExpire);
    }
}

}  // namespace streaming
