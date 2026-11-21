#include "net/EventLoopThreadPool.hpp"
#include "core/Logger.hpp"

#include <condition_variable>
#include <mutex>

namespace streaming {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : baseLoop_(baseLoop),
      name_(std::move(name)),
      numThreads_(0),
      next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() { stop(); }

void EventLoopThreadPool::stop() {
    for (auto& t : threads_) {
        if (t && t->thread && t->thread->joinable()) {
            if (t->loop) t->loop->quit();
            t->thread->join();
        }
    }
}

void EventLoopThreadPool::start(const ThreadInitCallback& cb) {
    if (started_) return;
    started_ = true;
    baseLoop_->assertInLoopThread();

    threads_.resize(numThreads_);
    loops_.reserve(numThreads_);

    for (int i = 0; i < numThreads_; ++i) {
        auto lt = std::make_unique<LoopThread>();
        LoopThread* raw = lt.get();

        auto mtx = std::make_shared<std::mutex>();
        auto cv = std::make_shared<std::condition_variable>();
        auto ready = std::make_shared<bool>(false);

        std::string tname = name_ + "#" + std::to_string(i);
        raw->thread = std::make_unique<std::thread>(
            [raw, mtx, cv, ready, cb, tname]() {
                EventLoop loop;
                {
                    std::lock_guard<std::mutex> lk(*mtx);
                    raw->loop = &loop;
                    *ready = true;
                }
                cv->notify_one();

                if (cb) cb(&loop);
                LOG_INFO("%s start looping", tname.c_str());
                loop.loop();
            });

        // 等待该线程把 loop 指针准备好
        {
            std::unique_lock<std::mutex> lk(*mtx);
            cv->wait(lk, [&] { return *ready; });
        }
        loops_.push_back(raw->loop);
        threads_[i] = std::move(lt);
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    baseLoop_->assertInLoopThread();
    if (loops_.empty()) return baseLoop_;
    size_t idx = next_.fetch_add(1) % loops_.size();
    return loops_[idx];
}

EventLoop* EventLoopThreadPool::getLoopForHash(size_t hashCode) {
    if (loops_.empty()) return baseLoop_;
    return loops_[hashCode % loops_.size()];
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() const {
    if (loops_.empty()) {
        return {baseLoop_};
    }
    return loops_;
}

}  // namespace streaming
