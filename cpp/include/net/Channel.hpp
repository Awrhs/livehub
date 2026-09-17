#pragma once

#include "core/NonCopyable.hpp"
#include "core/Timestamp.hpp"

#include <functional>

namespace streaming {

class EventLoop;

/**
 * Channel: 一个 fd + 其关心的事件 + 事件回调。
 *
 * Channel 不拥有 fd（fd 归 Socket/EventFd 所有），它只负责把 fd 的事件
 * 分发给回调函数。这是 Reactor 模式的核心抽象。
 *
 * 事件类型：
 * - kReadEvent  : 可读
 * - kWriteEvent : 可写
 * - kErrorEvent : 错误
 * - kCloseEvent : 对端关闭
 */
class Channel : NonCopyable {
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(Timestamp)>;

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;
    static const int kErrorEvent;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    EventLoop* ownerLoop() const { return loop_; }
    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int revt) { revents_ = revt; }
    bool isNoneEvent() const { return events_ == kNoneEvent; }

    /// 设置读回调
    void setReadCallback(ReadEventCallback cb) {
        readCallback_ = std::move(cb);
    }
    void setWriteCallback(EventCallback cb) {
        writeCallback_ = std::move(cb);
    }
    void setCloseCallback(EventCallback cb) {
        closeCallback_ = std::move(cb);
    }
    void setErrorCallback(EventCallback cb) {
        errorCallback_ = std::move(cb);
    }

    /// 启用读关注
    void enableReading() { events_ |= kReadEvent; update(); }
    void disableReading() { events_ &= ~kReadEvent; update(); }
    void enableWriting() { events_ |= kWriteEvent; update(); }
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    void disableAll() { events_ = kNoneEvent; update(); }
    bool isWriting() const { return events_ & kWriteEvent; }
    bool isReading() const { return events_ & kReadEvent; }

    /// 从 poller 移除自己
    void remove();

    /// 处理就绪事件（epoll_wait 返回后调用）
    void handleEvent(Timestamp receiveTime);

    /// 注册/更新到 poller
    void update();

private:
    EventLoop* loop_;
    int fd_;
    int events_;   // 关心的事件
    int revents_;  // poller 返回的就绪事件
    int index_;    // 在 poller 中的状态：-1=未添加，0=已添加，1=已修改

    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;

    friend class EpollPoller;
};

}  // namespace streaming
