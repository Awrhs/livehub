#pragma once

#include "core/NonCopyable.hpp"
#include "transcode/ProcessManager.hpp"
#include "transcode/Profile.hpp"
#include "transcode/Transcoder.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace streaming {

class EventLoop;

/// 转码任务生命周期状态
enum class TaskState { kPending, kRunning, kCompleted, kFailed, kCancelled };

/// 状态名（API/日志用）
inline const char* taskStateName(TaskState s) {
    switch (s) {
        case TaskState::kPending:   return "pending";
        case TaskState::kRunning:   return "running";
        case TaskState::kCompleted: return "completed";
        case TaskState::kFailed:    return "failed";
        case TaskState::kCancelled: return "cancelled";
    }
    return "unknown";
}

/**
 * TranscodeTask: 一路转码任务的状态。
 */
struct TranscodeTask {
    std::string id;
    std::string inputUrl;
    std::string outputPrefix;     // e.g. rtmp://localhost/live/<key>_<profile>
    std::vector<Profile> profiles;
    bool useLibav = true;         // false 时 fork 外部 ffmpeg
    int priority = 0;             // 大者先调度
    uint64_t seq = 0;             // 提交序号：同优先级 FIFO 的稳定 tiebreak

    TaskState state = TaskState::kPending;
    std::string error;            // kFailed 时的原因
    int ffmpegPid = -1;
    std::shared_ptr<Transcoder> transcoder;
    double startedAtSec = 0;      // steady 秒（运行中任务显示运行时长）
    int64_t submittedAtMs = 0;    // epoch 毫秒
    int64_t startedAtMs = 0;
    int64_t endedAtMs = 0;
};

/// 任务统计（GET /api/transcode 与 Prometheus gauge 的数据源）
struct TranscodeStats {
    size_t pending = 0;
    size_t running = 0;
    size_t completed = 0;
    size_t failed = 0;
    size_t cancelled = 0;
    double avgWaitMs = 0.0;       // 已启动任务的平均排队时长
};

/**
 * TranscodeManager: 管理转码任务全生命周期。
 *
 * - 并发上限（默认 hardware_concurrency），超出入队等待。
 * - 优先级队列：priority 大者先调度，同优先级按提交序号 FIFO。
 * - 状态机 kPending→kRunning→kCompleted/kFailed；任意时刻可 kCancelled。
 * - 终态任务归档保留（TTL 清理 + 数量上限），供 API 查询与统计。
 * - CPU 高水位保护（读 /proc/stat 计算整体使用率，SystemStats）。
 * - 支持两种后端：内建 libav（Transcoder）或外部 ffmpeg（ProcessManager）。
 */
class TranscodeManager : NonCopyable {
public:
    explicit TranscodeManager(EventLoop* loop,
                               int maxConcurrent = 0);  // 0=auto
    ~TranscodeManager();

    /// 终态任务保留数量上限（超出丢最旧）
    static constexpr size_t kMaxFinished = 1000;

    /// 启动调度线程
    void start();
    void stop();

    /// 提交任务，返回任务 id（同步生成，异步执行）
    std::string submit(const std::string& inputUrl,
                       const std::string& outputPrefix,
                       std::vector<Profile> profiles,
                       bool useLibav = true, int priority = 0);

    /// 取消任务：排队中的移出队列；运行中的停止转码器/进程。
    /// 返回是否找到了该任务。
    bool cancel(const std::string& id);

    /// 全量任务快照（排队 + 运行 + 近期终态），按调度/完成顺序。
    std::vector<TranscodeTask> list() const;
    size_t activeCount() const;
    size_t queuedCount() const;
    TranscodeStats stats() const;

    /// 清理结束时间早于 now-ttlSec 的终态任务（并执行数量上限淘汰）。
    /// ttlSec<=0 视为全部清理。返回清理条数。
    size_t sweepFinished(int ttlSec);

    /// CPU 使用率（0..1），从 /proc/stat 计算（SystemStats 差分）
    double cpuUsage();

    void setMaxConcurrent(int n) { maxConcurrent_ = n > 0 ? n : 1; }

    /// 输出地址追加的查询串（如 "?internal=<token>"）：转码结果推回本机时，
    /// 业务平台据此识别为内部推流并放行（on_publish webhook 收到的 param）
    void setOutputQuery(std::string q) { outputQuery_ = std::move(q); }

private:
    std::string outputQuery_;
    void schedulerLoop();
    /// 执行任务；返回 false 表示失败（原因写入 task->error）
    bool runTask(const std::shared_ptr<TranscodeTask>& task);
    void buildFfmpegArgs(const TranscodeTask& task,
                         std::vector<std::string>& argv) const;
    /// 归档终态任务（调用方持锁）
    void archiveFinishedLocked(const std::shared_ptr<TranscodeTask>& task);

    [[maybe_unused]] EventLoop* loop_;
    int maxConcurrent_;
    std::unique_ptr<ProcessManager> procManager_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<TranscodeTask>> queue_;  // priority desc, seq asc
    std::map<std::string, std::shared_ptr<TranscodeTask>> active_;
    std::deque<std::shared_ptr<TranscodeTask>> finished_;  // 完成顺序
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace streaming
