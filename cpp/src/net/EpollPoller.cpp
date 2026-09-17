#include "net/EpollPoller.hpp"
#include "net/Channel.hpp"
#include "net/EventLoop.hpp"
#include "core/Logger.hpp"

#include <unistd.h>
#include <errno.h>
#include <string.h>

namespace streaming {

EpollPoller::EpollPoller(EventLoop* ownerLoop)
    : epollFd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize),
      ownerLoop_(ownerLoop) {
    if (epollFd_ < 0) {
        LOG_FATAL("epoll_create1 failed: %s", strerror(errno));
    }
}

EpollPoller::~EpollPoller() {
    if (epollFd_ >= 0) {
        ::close(epollFd_);
    }
}

bool EpollPoller::hasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

Timestamp EpollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    int numEvents = ::epoll_wait(epollFd_,
                                 events_.data(),
                                 static_cast<int>(events_.size()),
                                 timeoutMs);
    int savedErrno = errno;
    Timestamp now(Timestamp::now());

    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);
        // 事件数组满了，扩容以适应更多连接
        if (static_cast<size_t>(numEvents) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (numEvents == 0) {
        // 超时，无事件
    } else {
        if (savedErrno != EINTR) {
            errno = savedErrno;
            LOG_ERROR("epoll_wait error: %s", strerror(savedErrno));
        }
    }
    return now;
}

void EpollPoller::fillActiveChannels(int numEvents,
                                     ChannelList* activeChannels) const {
    for (int i = 0; i < numEvents; ++i) {
        Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel);
    }
}

void EpollPoller::updateChannel(Channel* channel) {
    const int index = channel->index_;  // 友元访问
    if (index == -1 || index == 2) {
        // 新增
        int fd = channel->fd();
        if (channels_.find(fd) == channels_.end()) {
            channels_[fd] = channel;
        }
        channel->index_ = 1;  // 已添加
        update(EPOLL_CTL_ADD, channel);
    } else {
        // 已存在，修改或删除
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->index_ = 2;  // 已删除
        } else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

void EpollPoller::removeChannel(Channel* channel) {
    int fd = channel->fd();
    if (channels_.erase(fd) == 0) return;

    if (channel->index_ != 2) {
        // 还在 epoll 中，先删除
        update(EPOLL_CTL_DEL, channel);
    }
    channel->index_ = -1;
}

void EpollPoller::update(int operation, Channel* channel) {
    struct epoll_event event;
    ::memset(&event, 0, sizeof(event));
    event.events = static_cast<uint32_t>(channel->events());
    event.data.ptr = channel;
    int fd = channel->fd();

    if (::epoll_ctl(epollFd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            LOG_ERROR("epoll_ctl DEL fd=%d error: %s", fd, strerror(errno));
        } else {
            LOG_FATAL("epoll_ctl op=%d fd=%d error: %s",
                      operation, fd, strerror(errno));
        }
    }
}

}  // namespace streaming
