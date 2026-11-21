#pragma once

#include "core/NonCopyable.hpp"
#include "net/EventLoop.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace streaming {

/**
 * EventLoopThreadPool: subReactor 线程池。
 *
 * one loop per thread：池内每个线程运行一个独立的 EventLoop。
 * baseLoop（主线程）负责 accept，新连接轮询分配到某个 subLoop。
 *
 * 简历亮点：mainReactor + subReactor 经典多线程 Reactor 模型
 */
class EventLoopThreadPool : NonCopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThreadPool(EventLoop* baseLoop, std::string name = "subLoop");
    ~EventLoopThreadPool();

    /// 设置 subLoop 线程数；0 表示不启用（所有连接都在 baseLoop）
    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    /// 启动所有 subLoop 线程
    void start(const ThreadInitCallback& cb = ThreadInitCallback());
    /// 退出所有 IO 线程并 join（幂等）。已投递的任务会在退出前执行完。
    void stop();

    /// round-robin 获取下一个 subLoop（无 pool 时返回 baseLoop）
    EventLoop* getNextLoop();

    /// 按哈希固定映射到某个 subLoop（同 key 连接落同 loop，缓存亲和）
    EventLoop* getLoopForHash(size_t hashCode);

    std::vector<EventLoop*> getAllLoops() const;
    bool started() const { return started_; }

private:
    EventLoop* baseLoop_;
    std::string name_;
    bool started_ = false;
    int numThreads_;
    std::atomic<size_t> next_;

    // 线程与 loop 一一对应。EventLoop 在线程内创建（绑定线程 id），但由池持有、
    // 在 join 之后才销毁：否则 quit() 的唤醒写入会与线程退出时关闭 eventfd 竞争。
    struct LoopThread {
        EventLoop* loop = nullptr;
        std::unique_ptr<EventLoop> owned;
        std::unique_ptr<std::thread> thread;
    };
    std::vector<std::unique_ptr<LoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace streaming
