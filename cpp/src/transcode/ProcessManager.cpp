#include "transcode/ProcessManager.hpp"

#include "core/Logger.hpp"
#include "net/Channel.hpp"
#include "net/EventLoop.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

extern "C" char** environ;

namespace streaming {

namespace {
int setNonBlockCloexec(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(fd, F_GETFD, 0);
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    return fd;
}
}  // namespace

ProcessManager::ProcessManager(EventLoop* loop) : loop_(loop) {
    // 屏蔽 SIGCHLD，由 signalfd 接管
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    sigchldFd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigchldFd_ >= 0) {
        sigchldChannel_ =
            std::make_shared<Channel>(loop_, sigchldFd_);
        sigchldChannel_->setReadCallback([this](Timestamp) { onSigchld(); });
        sigchldChannel_->enableReading();
    } else {
        LOG_ERROR("ProcessManager: signalfd failed: %s", strerror(errno));
    }
}

ProcessManager::~ProcessManager() {
    if (sigchldChannel_) {
        sigchldChannel_->disableAll();
        sigchldChannel_->remove();
    }
    if (sigchldFd_ >= 0) ::close(sigchldFd_);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [pid, p] : procs_) {
        if (p->state == ProcState::kRunning) {
            ::kill(pid, SIGKILL);
        }
    }
}

pid_t ProcessManager::spawn(const std::string& name,
                            const std::vector<std::string>& argv,
                            int timeoutSec) {
    if (argv.empty()) return -1;

    int errpipe[2];
    if (pipe2(errpipe, O_NONBLOCK | O_CLOEXEC) < 0) {
        LOG_ERROR("ProcessManager: pipe2 failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("ProcessManager: fork failed: %s", strerror(errno));
        ::close(errpipe[0]);
        ::close(errpipe[1]);
        return -1;
    }

    if (pid == 0) {
        // 子进程：恢复默认信号处理
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigprocmask(SIG_UNBLOCK, &mask, nullptr);

        ::dup2(errpipe[1], STDERR_FILENO);
        ::close(errpipe[0]);
        if (errpipe[1] != STDERR_FILENO) ::close(errpipe[1]);

        std::vector<char*> cargs;
        cargs.reserve(argv.size() + 1);
        for (const auto& a : argv) {
            cargs.push_back(const_cast<char*>(a.c_str()));
        }
        cargs.push_back(nullptr);
        execvp(cargs[0], cargs.data());
        // exec 失败
        _exit(127);
    }

    // 父进程
    ::close(errpipe[1]);
    int readFd = errpipe[0];
    setNonBlockCloexec(readFd);

    auto proc = std::make_shared<Process>();
    proc->pid = pid;
    proc->name = name;
    proc->state = ProcState::kRunning;
    proc->startedAt = std::chrono::steady_clock::now();
    proc->stderrFd = readFd;

    auto weakProc = std::weak_ptr<Process>(proc);
    proc->stderrChannel = std::make_shared<Channel>(loop_, readFd);
    proc->stderrChannel->setReadCallback(
        [this, pid](Timestamp) { onStderrRead(pid); });
    proc->stderrChannel->enableReading();

    if (timeoutSec > 0) {
        auto tid = loop_->runAfter(static_cast<double>(timeoutSec),
                                   [this, pid] { onTimeout(pid); });
        proc->timerId = 0;  // TimerId 由 loop 持有；简化记录
        (void)tid;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        procs_[pid] = proc;
    }
    LOG_INFO("ProcessManager: spawned pid=%d name=%s", pid, name.c_str());
    return pid;
}

void ProcessManager::onStderrRead(pid_t pid) {
    std::shared_ptr<Process> proc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = procs_.find(pid);
        if (it == procs_.end()) return;
        proc = it->second;
    }
    char buf[4096];
    while (true) {
        ssize_t n = ::read(proc->stderrFd, buf, sizeof(buf));
        if (n > 0) {
            proc->stderrBytes += static_cast<size_t>(n);
            proc->stderrBuf.append(buf, static_cast<size_t>(n));
            // 按行切分
            size_t pos = 0;
            while (true) {
                auto nl = proc->stderrBuf.find('\n', pos);
                if (nl == std::string::npos) {
                    if (pos > 0) proc->stderrBuf.erase(0, pos);
                    break;
                }
                std::string line = proc->stderrBuf.substr(pos, nl - pos);
                pos = nl + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (onLog_) onLog_(pid, line);
            }
        } else if (n == 0 || (n < 0 && errno != EINTR)) {
            // 写端关闭 / 非阻塞无数据
            break;
        }
    }
}

void ProcessManager::onSigchld() {
    struct signalfd_siginfo si;
    while (::read(sigchldFd_, &si, sizeof(si)) == sizeof(si)) {
        // 一次 signalfd 可能聚合多个子进程退出，统一 reap
    }
    reapAll();
}

void ProcessManager::reapAll() {
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        handleExit(pid, status);
    }
}

void ProcessManager::handleExit(pid_t pid, int status) {
    std::shared_ptr<Process> proc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = procs_.find(pid);
        if (it == procs_.end()) return;
        proc = it->second;
        procs_.erase(it);
    }
    if (WIFEXITED(status)) {
        proc->exitCode = WEXITSTATUS(status);
        proc->state = proc->exitCode == 0 ? ProcState::kExited
                                           : ProcState::kFailed;
    } else if (WIFSIGNALED(status)) {
        proc->exitCode = 128 + WTERMSIG(status);
        proc->state = ProcState::kKilled;
    }
    if (proc->stderrChannel) {
        proc->stderrChannel->disableAll();
        proc->stderrChannel->remove();
    }
    if (proc->stderrFd >= 0) ::close(proc->stderrFd);
    LOG_INFO("ProcessManager: pid=%d exited code=%d", pid, proc->exitCode);
    if (onExit_) onExit_(pid, proc->exitCode, proc->state);
}

void ProcessManager::onTimeout(pid_t pid) {
    LOG_WARN("ProcessManager: pid=%d timeout, killing", pid);
    kill(pid, SIGKILL);
}

void ProcessManager::kill(pid_t pid, int signo) {
    if (pid > 0) ::kill(pid, signo);
}

void ProcessManager::terminate(pid_t pid) { kill(pid, SIGTERM); }

ProcStats ProcessManager::stats(pid_t pid) const {
    ProcStats s;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = procs_.find(pid);
    if (it != procs_.end()) {
        const auto& p = it->second;
        s.pid = p->pid;
        s.name = p->name;
        s.state = p->state;
        s.exitCode = p->exitCode;
        s.startedAt = p->startedAt;
        s.stderrBytes = p->stderrBytes;
    }
    return s;
}

std::vector<ProcStats> ProcessManager::list() const {
    std::vector<ProcStats> out;
    std::lock_guard<std::mutex> lk(mu_);
    out.reserve(procs_.size());
    for (const auto& [pid, p] : procs_) {
        ProcStats s;
        s.pid = p->pid;
        s.name = p->name;
        s.state = p->state;
        s.exitCode = p->exitCode;
        s.startedAt = p->startedAt;
        s.stderrBytes = p->stderrBytes;
        out.push_back(s);
    }
    return out;
}

size_t ProcessManager::runningCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& [pid, p] : procs_) {
        if (p->state == ProcState::kRunning) ++n;
    }
    return n;
}

}  // namespace streaming
