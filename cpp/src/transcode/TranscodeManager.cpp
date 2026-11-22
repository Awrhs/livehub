#include "transcode/TranscodeManager.hpp"

#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "metrics/Metrics.hpp"
#include "metrics/SystemStats.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace streaming {

namespace {
std::string nextTaskId() {
    static std::atomic<uint64_t> ctr{0};
    uint64_t n = ctr.fetch_add(1);
    std::ostringstream os;
    os << "tc-" << n;
    return os.str();
}

uint64_t nextSeq() {
    static std::atomic<uint64_t> seq{0};
    return seq.fetch_add(1);
}

int64_t nowMs() {
    return Timestamp::now().microSecondsSinceEpoch() / 1000;
}
}  // namespace

TranscodeManager::TranscodeManager(EventLoop* loop, int maxConcurrent)
    : loop_(loop),
      maxConcurrent_(maxConcurrent > 0
                          ? maxConcurrent
                          : static_cast<int>(
                                std::thread::hardware_concurrency())),
      procManager_(loop ? std::make_unique<ProcessManager>(loop) : nullptr) {}

TranscodeManager::~TranscodeManager() {
    stop();
}

void TranscodeManager::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread([this] { schedulerLoop(); });
}

void TranscodeManager::stop() {
    running_.store(false);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, t] : active_) {
        if (t->transcoder) t->transcoder->stop();
        if (t->ffmpegPid > 0 && procManager_) procManager_->kill(t->ffmpegPid, 9);
    }
    active_.clear();
    queue_.clear();
    finished_.clear();
}

std::string TranscodeManager::submit(const std::string& inputUrl,
                                      const std::string& outputPrefix,
                                      std::vector<Profile> profiles,
                                      bool useLibav, int priority) {
    auto task = std::make_shared<TranscodeTask>();
    task->id = nextTaskId();
    task->inputUrl = inputUrl;
    task->outputPrefix = outputPrefix;
    task->profiles = std::move(profiles);
    task->useLibav = useLibav;
    task->priority = priority;
    task->seq = nextSeq();
    task->submittedAtMs = nowMs();
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 有序插入：priority 降序，同优先级按 seq 升序（提交顺序 FIFO）。
        // 队列长度通常很小，线性插入足够。
        auto it = queue_.begin();
        while (it != queue_.end() &&
               ((*it)->priority > task->priority ||
                ((*it)->priority == task->priority &&
                 (*it)->seq < task->seq))) {
            ++it;
        }
        queue_.insert(it, task);
    }
    cv_.notify_one();
    LOG_INFO("TranscodeManager: task %s queued (input=%s priority=%d)",
             task->id.c_str(), inputUrl.c_str(), priority);
    return task->id;
}

bool TranscodeManager::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    // 从队列移除
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if ((*it)->id == id) {
            auto task = *it;
            queue_.erase(it);
            task->state = TaskState::kCancelled;
            task->endedAtMs = nowMs();
            archiveFinishedLocked(task);
            LOG_INFO("TranscodeManager: task %s cancelled (pending)",
                     id.c_str());
            return true;
        }
    }
    // 停止活跃任务；runTask 返回后由调度线程按 kCancelled 归档
    auto it = active_.find(id);
    if (it != active_.end()) {
        if (it->second->transcoder) it->second->transcoder->stop();
        if (it->second->ffmpegPid > 0 && procManager_) {
            procManager_->terminate(it->second->ffmpegPid);
        }
        it->second->state = TaskState::kCancelled;
        LOG_INFO("TranscodeManager: task %s cancelled (running)",
                 id.c_str());
        return true;
    }
    return false;
}

std::vector<TranscodeTask> TranscodeManager::list() const {
    std::vector<TranscodeTask> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& t : queue_) out.push_back(*t);
    for (const auto& [id, t] : active_) out.push_back(*t);
    for (const auto& t : finished_) out.push_back(*t);
    return out;
}

size_t TranscodeManager::activeCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_.size();
}

size_t TranscodeManager::queuedCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size();
}

TranscodeStats TranscodeManager::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    TranscodeStats s;
    s.pending = queue_.size();
    s.running = active_.size();
    int64_t waitSum = 0;
    size_t waitN = 0;
    for (const auto& [id, t] : active_) {
        if (t->startedAtMs > 0) {
            waitSum += t->startedAtMs - t->submittedAtMs;
            ++waitN;
        }
    }
    for (const auto& t : finished_) {
        switch (t->state) {
            case TaskState::kCompleted: ++s.completed; break;
            case TaskState::kFailed:    ++s.failed; break;
            case TaskState::kCancelled: ++s.cancelled; break;
            default: break;
        }
        if (t->startedAtMs > 0) {
            waitSum += t->startedAtMs - t->submittedAtMs;
            ++waitN;
        }
    }
    s.avgWaitMs = waitN ? static_cast<double>(waitSum) /
                              static_cast<double>(waitN)
                        : 0.0;
    return s;
}

size_t TranscodeManager::sweepFinished(int ttlSec) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t removed = 0;
    // finished_ 按完成顺序（endedAtMs 递增）追加，从头清理即可
    if (ttlSec <= 0) {
        removed = finished_.size();
        finished_.clear();
        return removed;
    }
    int64_t cutoff = nowMs() - static_cast<int64_t>(ttlSec) * 1000;
    while (!finished_.empty() && finished_.front()->endedAtMs <= cutoff) {
        finished_.pop_front();
        ++removed;
    }
    while (finished_.size() > kMaxFinished) {
        finished_.pop_front();
        ++removed;
    }
    return removed;
}

double TranscodeManager::cpuUsage() {
    // 差分采样统一走 SystemStats（与 /api/health、Prometheus gauge 共用一份状态）
    return SystemStats::instance().cpuUsage();
}

void TranscodeManager::archiveFinishedLocked(
    const std::shared_ptr<TranscodeTask>& task) {
    finished_.push_back(task);
    // 数量上限：丢最旧
    while (finished_.size() > kMaxFinished) finished_.pop_front();
    if (task->state == TaskState::kFailed) {
        Metrics::instance()
            .counter("transcode_tasks_failed_total", "Failed transcode tasks")
            ->inc();
    }
}

void TranscodeManager::schedulerLoop() {
    while (running_.load()) {
        std::shared_ptr<TranscodeTask> task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(500), [this] {
                return !running_.load() ||
                       (!queue_.empty() &&
                        static_cast<int>(active_.size()) < maxConcurrent_);
            });
            if (!running_.load()) break;
            if (queue_.empty() ||
                static_cast<int>(active_.size()) >= maxConcurrent_) {
                continue;
            }
            // CPU 高水位保护
            if (cpuUsage() > 0.9) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            task = queue_.front();  // 队头即最高优先级（submit 有序插入）
            queue_.pop_front();
            task->state = TaskState::kRunning;
            task->startedAtMs = nowMs();
            task->startedAtSec =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            active_[task->id] = task;
        }
        bool ok = runTask(task);
        {
            std::lock_guard<std::mutex> lk(mu_);
            active_.erase(task->id);
            task->endedAtMs = nowMs();
            if (task->state == TaskState::kCancelled) {
                // cancel() 已标记（运行中被取消）
            } else if (!ok) {
                task->state = TaskState::kFailed;
            } else {
                task->state = TaskState::kCompleted;
            }
            archiveFinishedLocked(task);
            LOG_INFO("TranscodeManager: task %s %s", task->id.c_str(),
                     taskStateName(task->state));
        }
        cv_.notify_one();
    }
}

namespace {
constexpr bool kLibavOutputWired = false;
}  // namespace

bool TranscodeManager::runTask(const std::shared_ptr<TranscodeTask>& task) {
    LOG_INFO("TranscodeManager: running task %s", task->id.c_str());

    // 进程内 libav 转码的输出从未接入分发链（输出回调为空），且无 libav 构建时直接“完成”。
    // 在接通之前统一使用外部 ffmpeg 进程。
    if (task->useLibav && kLibavOutputWired) {
        task->transcoder = std::make_shared<Transcoder>(
            task->inputUrl, task->profiles);
        task->transcoder->setOutputCallback(
            [](const EncodedPacket&) {
                // 输出送入 HlsSource / MediaStream（在 Stage 7 装配时绑定）
            });
        task->transcoder->start();
        // 同步等待完成
        while (task->transcoder->isRunning() && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        task->transcoder->stop();
        return true;
    }
    // fork 外部 ffmpeg
    std::vector<std::string> argv;
    buildFfmpegArgs(*task, argv);
    if (!procManager_) {
        task->error = "no process manager (no event loop)";
        return false;
    }
    pid_t pid = procManager_->spawn(task->id, argv, 0);
    if (pid <= 0) {
        task->error = "ffmpeg spawn failed";
        return false;
    }
    task->ffmpegPid = pid;
    // 轮询进程是否退出
    ProcStats st;
    while (running_.load()) {
        st = procManager_->stats(pid);
        if (st.state != ProcState::kRunning) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (st.state == ProcState::kExited && st.exitCode != 0) {
        task->error = "ffmpeg exited with code " + std::to_string(st.exitCode);
        return false;
    }
    if (st.state == ProcState::kFailed) {
        task->error = "ffmpeg failed to start";
        return false;
    }
    if (st.state == ProcState::kKilled) {
        task->error = "ffmpeg killed by signal";
        return false;
    }
    return true;
}

void TranscodeManager::buildFfmpegArgs(const TranscodeTask& task,
                                        std::vector<std::string>& argv) const {
    argv.push_back("ffmpeg");
    argv.push_back("-hide_banner");
    argv.push_back("-loglevel");
    argv.push_back("warning");
    argv.push_back("-i");
    argv.push_back(task.inputUrl);

    for (const auto& p : task.profiles) {
        if (p.videoBitrate > 0) {
            argv.push_back("-c:v");
            argv.push_back(p.videoCodec);
            argv.push_back("-preset");
            argv.push_back(p.preset);
            if (!p.tune.empty()) {
                argv.push_back("-tune");
                argv.push_back(p.tune);
            }
            if (p.width > 0 && p.height > 0) {
                argv.push_back("-s");
                argv.push_back(std::to_string(p.width) + "x" +
                               std::to_string(p.height));
            }
            argv.push_back("-b:v");
            argv.push_back(std::to_string(p.videoBitrate));
            argv.push_back("-maxrate");
            argv.push_back(std::to_string(static_cast<int>(
                p.videoBitrate * 1.2)));
            argv.push_back("-bufsize");
            argv.push_back(std::to_string(p.videoBitrate * 2));
            argv.push_back("-g");
            argv.push_back(std::to_string(p.gopSize > 0 ? p.gopSize : 60));
        }
        if (p.audioBitrate > 0) {
            argv.push_back("-c:a");
            argv.push_back(p.audioCodec);
            argv.push_back("-b:a");
            argv.push_back(std::to_string(p.audioBitrate));
        }
        // 输出：RTMP 推回本地（多码率 ABR）
        argv.push_back("-f");
        argv.push_back("flv");
        argv.push_back(task.outputPrefix + "_" + p.name + outputQuery_);
    }
}

}  // namespace streaming
