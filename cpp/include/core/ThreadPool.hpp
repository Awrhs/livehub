#pragma once

#include "core/NonCopyable.hpp"
#include "core/Logger.hpp"

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace streaming {

/**
 * ThreadPool: 固定大小线程池。
 *
 * 设计要点：
 * - 任务用 std::function<void()> 包装，支持任意返回值（通过 packaged_task）
 * - 提交任务返回 std::future，可获取结果
 * - 优雅关闭：stop_ 标志唤醒所有线程
 *
 * 简历亮点：标准库线程池实现，理解 future/promise 与条件变量
 */
class ThreadPool : NonCopyable {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t numThreads = 0)
        : stop_(false) {
        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads == 0) numThreads = 4;
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
        LOG_INFO("ThreadPool started with %zu threads", numThreads);
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            stop_ = true;
        }
        condition_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    /// 提交任务（返回 future 以获取结果）
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using ReturnType = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<ReturnType> result = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stop_) {
                throw std::runtime_error("ThreadPool stopped");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return result;
    }

    size_t queueSize() const {
        std::unique_lock<std::mutex> lock(queueMutex_);
        return tasks_.size();
    }

private:
    void workerLoop() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                condition_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            try {
                task();
            } catch (const std::exception& e) {
                LOG_ERROR("ThreadPool task exception: %s", e.what());
            } catch (...) {
                LOG_ERROR("ThreadPool task unknown exception");
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    mutable std::mutex queueMutex_;
    std::condition_variable condition_;
    bool stop_;
};

}  // namespace streaming
