#pragma once

#include "core/NonCopyable.hpp"
#include "net/Channel.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace streaming {

class EventLoop;

/**
 * SignalGuard: 用 signalfd 捕获 SIGINT/SIGTERM（及可选 SIGCHLD），
 * 注册到 EventLoop，收到信号时触发关闭回调，实现优雅退出。
 *
 * 流程：
 *   收到 SIGINT/SIGTERM → onSignal_ 回调（停止 Acceptor、关闭空闲连接…）
 *   回调返回后调用 loop_->quit()。
 */
class SignalGuard : NonCopyable {
public:
    using SignalCallback = std::function<void(int signo)>;

    explicit SignalGuard(EventLoop* loop);
    ~SignalGuard();

    /// 追加监听信号（SIGINT/SIGTERM 默认已监听）
    void addSignal(int signo);

    void setSignalCallback(SignalCallback cb) { cb_ = std::move(cb); }

private:
    void onRead();

    EventLoop* loop_;
    int sigFd_ = -1;
    std::shared_ptr<Channel> channel_;
    SignalCallback cb_;
    std::vector<int> signals_;
};

}  // namespace streaming
