#include "net/Channel.hpp"
#include "net/EventLoop.hpp"
#include "core/Logger.hpp"

#include <poll.h>

namespace streaming {

// epoll 事件位
// EPOLLIN=0x001, EPOLLOUT=0x004, EPOLLRDHUP=0x2000, EPOLLPRI=0x002, EPOLLERR=0x008, EPOLLHUP=0x010
const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const int Channel::kWriteEvent = EPOLLOUT;
const int Channel::kErrorEvent = 0;  // EPOLLERR/EPOLLHUP 总是会被报告

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop),
      fd_(fd),
      events_(kNoneEvent),
      revents_(0),
      index_(-1) {}

Channel::~Channel() {
    // 析构时不 close fd（fd 由所有者管理）
}

void Channel::update() {
    // 委托给 EventLoop -> Poller
    loop_->updateChannel(this);
}

void Channel::remove() {
    events_ = kNoneEvent;
    loop_->removeChannel(this);
}

void Channel::handleEvent(Timestamp receiveTime) {
    // EPOLLHUP: 对端关闭（写端或全关闭）
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (closeCallback_) closeCallback_();
        return;
    }

    // EPOLLERR: 出错
    if (revents_ & EPOLLERR) {
        if (errorCallback_) errorCallback_();
    }

    // EPOLLRDHUP: 对端关闭连接（半关闭）
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (readCallback_) readCallback_(receiveTime);
    }

    // EPOLLOUT: 可写
    if (revents_ & EPOLLOUT) {
        if (writeCallback_) writeCallback_();
    }
}

}  // namespace streaming
