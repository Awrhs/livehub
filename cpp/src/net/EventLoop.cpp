#include "net/EventLoop.hpp"
#include "net/Channel.hpp"
#include "net/TimerQueue.hpp"
#include "core/Logger.hpp"

#include <unistd.h>
#include <errno.h>
#include <string.h>

namespace streaming {

// 全局：创建 eventfd
static int createEventfd() {
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0) {
        LOG_FATAL("eventfd create failed: %s", strerror(errno));
    }
    return evtfd;
}

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      threadId_(std::this_thread::get_id()),
      poller_(new EpollPoller(this)),
      timerQueue_(new TimerQueue(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_)),
      callingPending_(false) {
    LOG_DEBUG("EventLoop created in tid=%zu",
              std::hash<std::thread::id>{}(threadId_));
    // 设置 wakeup channel 的读回调，并启用读
    wakeupChannel_->setReadCallback(
        [this](Timestamp) { this->handleRead(); });
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
}

void EventLoop::loop() {
    looping_ = true;
    quit_ = false;
    LOG_INFO("EventLoop start looping in tid=%zu",
             std::hash<std::thread::id>{}(threadId_));

    while (!quit_) {
        activeChannels_.clear();
        Timestamp pollReturnTime = poller_->poll(10000, &activeChannels_);

        Timestamp start = Timestamp::now();
        for (Channel* channel : activeChannels_) {
            channel->handleEvent(pollReturnTime);
        }
        eventHandlingTime_ = Timestamp::now() - start;

        doPendingFunctors();
    }

    LOG_INFO("EventLoop stop looping");
    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    // 若在别的线程调用 quit，需唤醒本 loop
    if (!isInLoopThread()) {
        // 写 eventfd 唤醒
        uint64_t one = 1;
        ::write(wakeupFd_, &one, sizeof(one));
    }
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(std::move(cb));
    }
    // 唤醒 loop：
    // 1. 若不在 loop 线程
    // 2. 若正在 doPendingFunctors（本轮已执行完，但新任务需下轮）
    if (!isInLoopThread() || callingPending_) {
        uint64_t one = 1;
        ::write(wakeupFd_, &one, sizeof(one));
    }
}

void EventLoop::handleRead() {
    uint64_t one = 0;
    ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        LOG_ERROR("EventLoop::handleRead reads %zd bytes", n);
    }
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPending_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (const auto& fn : functors) {
        try {
            fn();
        } catch (const std::exception& e) {
            LOG_ERROR("EventLoop pending functor exception: %s", e.what());
        } catch (...) {
            LOG_ERROR("EventLoop pending functor unknown exception");
        }
    }
    callingPending_ = false;
}

void EventLoop::updateChannel(Channel* channel) {
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel) {
    return poller_->hasChannel(channel);
}

TimerId EventLoop::runAfter(double delaySeconds, std::function<void()> cb) {
    Timestamp when = addTime(Timestamp::now(), delaySeconds);
    return timerQueue_->addTimer(std::move(cb), when, 0.0);
}

TimerId EventLoop::runEvery(double intervalSeconds, std::function<void()> cb) {
    Timestamp when = addTime(Timestamp::now(), intervalSeconds);
    return timerQueue_->addTimer(std::move(cb), when, intervalSeconds);
}

void EventLoop::cancel(TimerId id) {
    timerQueue_->cancel(id);
}

}  // namespace streaming
