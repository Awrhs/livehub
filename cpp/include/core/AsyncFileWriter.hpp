#pragma once

#include "core/NonCopyable.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace streaming {

/**
 * AsyncFileWriter: 单后台线程执行文件 IO（open/write/fsync/close），
 * 避免 HLS/DVR/日志的磁盘写阻塞 Reactor 线程。
 *
 * 用法：
 *   AsyncFileWriter writer;
 *   writer.start();
 *   uint64_t id = writer.open("/path/to/seg.ts", [](bool ok){...});
 *   writer.append(id, data, len);   // 拷贝到队列后立即返回
 *   writer.close(id);               // close 后排入队列，回调在 IO 线程触发
 *
 * 多个文件可同时打开（用 fd 区分）。所有操作串行在后台线程。
 *
 * 简历点：异步 IO、生产者-消费者队列、背压（队列上限）、RAII flush。
 */
class AsyncFileWriter : NonCopyable {
public:
    using CloseCallback = std::function<void(bool success, uint64_t bytesWritten)>;

    explicit AsyncFileWriter(size_t maxQueueBytes = 64 * 1024 * 1024);
    ~AsyncFileWriter();

    void start();
    void stop();  // 排空队列后退出

    /// 打开文件（截断写）。返回文件句柄 id（>0）。
    uint64_t open(const std::string& path, bool append = false);

    /// 追加数据（内部拷贝）。线程安全。
    void append(uint64_t id, const uint8_t* data, size_t len);
    void append(uint64_t id, const std::vector<uint8_t>& data) {
        append(id, data.data(), data.size());
    }

    /// 请求 fsync
    void fsync(uint64_t id);

    /// 关闭文件；关闭完成后在 IO 线程触发 cb（字节数统计）。
    void close(uint64_t id, CloseCallback cb = nullptr);

    /// 删除文件（排入队列执行，unlink）
    void unlink(const std::string& path);

    uint64_t pendingBytes() const { return pendingBytes_.load(); }
    uint64_t droppedBytes() const { return droppedBytes_.load(); }
    uint64_t totalWritten() const { return totalWritten_.load(); }

private:
    enum class OpType { kOpen, kAppend, kFsync, kClose, kUnlink };
    struct Op {
        OpType type;
        uint64_t id = 0;
        std::string path;
        std::vector<uint8_t> data;
        bool appendMode = false;
        CloseCallback cb;
    };

    void run();
    void apply(Op& op);

    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Op> queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    size_t maxQueueBytes_;
    std::atomic<uint64_t> pendingBytes_{0};
    std::atomic<uint64_t> droppedBytes_{0};
    std::atomic<uint64_t> totalWritten_{0};
    std::atomic<uint64_t> nextId_{1};

    // 仅在 worker 线程访问
    struct OpenFile {
        int fd = -1;
        uint64_t written = 0;
    };
    std::unordered_map<uint64_t, OpenFile> files_;
};

}  // namespace streaming
