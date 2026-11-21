#include "core/SignalGuard.hpp"

#include "core/Logger.hpp"
#include "net/EventLoop.hpp"

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cstring>

namespace streaming {

SignalGuard::SignalGuard(EventLoop* loop) : loop_(loop) {
    sigset_t mask;
    sigemptyset(&mask);
    // 默认监听 SIGINT/SIGTERM
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    sigFd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigFd_ < 0) {
        LOG_ERROR("SignalGuard: signalfd failed: %s", strerror(errno));
        return;
    }
    channel_ = std::make_shared<Channel>(loop_, sigFd_);
    channel_->setReadCallback([this](Timestamp) { onRead(); });
    channel_->enableReading();
    signals_.push_back(SIGINT);
    signals_.push_back(SIGTERM);
}

SignalGuard::~SignalGuard() {
    if (channel_) {
        channel_->disableAll();
        channel_->remove();
    }
    if (sigFd_ >= 0) ::close(sigFd_);

    // 恢复默认信号处理
    sigset_t mask;
    sigemptyset(&mask);
    for (int s : signals_) sigaddset(&mask, s);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
}

void SignalGuard::addSignal(int signo) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signo);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    signalfd(sigFd_, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    signals_.push_back(signo);
}

void SignalGuard::onRead() {
    struct signalfd_siginfo si;
    while (::read(sigFd_, &si, sizeof(si)) == sizeof(si)) {
        LOG_INFO("SignalGuard: received signal %d", si.ssi_signo);
        if (cb_) cb_(si.ssi_signo);
        // 仅终止类信号触发退出；SIGHUP 等交由回调做热加载，不退出 loop。
        if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
            loop_->quit();
        }
    }
}

}  // namespace streaming