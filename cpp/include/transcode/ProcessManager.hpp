#pragma once

#include "core/NonCopyable.hpp"

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

class EventLoop;
class Channel;

/**
 * Process state.
 */
enum class ProcState { kRunning, kExited, kKilled, kFailed };

struct ProcStats {
    pid_t pid = 0;
    std::string name;
    ProcState state = ProcState::kRunning;
    int exitCode = -1;
    std::chrono::steady_clock::time_point startedAt;
    size_t stderrBytes = 0;
};

/**
 * ProcessManager: fork/exec 外部进程（兜底用 ffmpeg），非阻塞读 stderr，
 * 子进程退出时 SIGCHLD（signalfd）+ waitpid 回收。
 *
 * 设计：
 * - spawn(argv) 后 fork，子进程重定向 stderr 到 pipe（O_NONBLOCK|O_CLOEXEC），
 *   execvp；父进程把 stderr 读端注册到 EventLoop，非阻塞读日志。
 * - 创建 signalfd 监听 SIGCHLD 并注册到 loop，可读时 waitpid(-1, WNOHANG)
 *   批量回收，更新状态并触发回调。
 * - timeout：loop runAfter 定时 kill 超时进程。
 * - kill(pid, SIGTERM→SIGKILL)。
 *
 * 线程模型：所有操作在 loop 线程。
 */
class ProcessManager : NonCopyable {
public:
    using ExitCallback =
        std::function<void(pid_t pid, int exitCode, ProcState state)>;
    using LogCallback =
        std::function<void(pid_t pid, const std::string& line)>;

    explicit ProcessManager(EventLoop* loop);
    ~ProcessManager();

    /// 启动进程。argv[0] 为可执行文件；timeoutSec<=0 表示不超时。
    /// 返回子进程 pid；失败返回 -1。
    pid_t spawn(const std::string& name,
                const std::vector<std::string>& argv,
                int timeoutSec = 0);

    void kill(pid_t pid, int signo);
    void terminate(pid_t pid);  // SIGTERM

    ProcStats stats(pid_t pid) const;
    std::vector<ProcStats> list() const;

    void setExitCallback(ExitCallback cb) { onExit_ = std::move(cb); }
    void setLogCallback(LogCallback cb) { onLog_ = std::move(cb); }

    size_t runningCount() const;

private:
    struct Process {
        pid_t pid = 0;
        std::string name;
        ProcState state = ProcState::kRunning;
        int exitCode = -1;
        std::chrono::steady_clock::time_point startedAt;
        int stderrFd = -1;
        std::shared_ptr<Channel> stderrChannel;
        std::string stderrBuf;
        size_t stderrBytes = 0;
        uint64_t timerId = 0;
    };

    void onSigchld();
    void reapAll();
    void onStderrRead(pid_t pid);
    void onTimeout(pid_t pid);
    void handleExit(pid_t pid, int status);

    EventLoop* loop_;
    int sigchldFd_ = -1;
    std::shared_ptr<Channel> sigchldChannel_;
    mutable std::mutex mu_;
    std::map<pid_t, std::shared_ptr<Process>> procs_;

    ExitCallback onExit_;
    LogCallback onLog_;
};

}  // namespace streaming
