#pragma once

#include "core/NonCopyable.hpp"
#include "core/Timestamp.hpp"

#include <sys/epoll.h>
#include <unordered_map>
#include <vector>

namespace streaming {

class Channel;
class EventLoop;

/**
 * EpollPoller: epoll 的封装（ET 模式）。
 *
 * 设计要点：
 * - 维护 fd -> Channel* 的映射
 * - updateChannel: 添加/修改 fd 关心的事件
 * - poll: epoll_wait，返回就绪 Channel 列表
 * - 使用水平触发(LT)为主，避免 ET 模式下漏读的复杂性
 *   （后续 TS 分发等热路径可单独用 ET + sendfile）
 *
 * 简历亮点：epoll 封装，理解 LT/ET、事件分发
 */
class EpollPoller : NonCopyable {
public:
    using ChannelList = std::vector<Channel*>;

    explicit EpollPoller(EventLoop* ownerLoop);
    ~EpollPoller();

    /// 阻塞等待事件，超时毫秒。返回就绪的 Channel 列表
    Timestamp poll(int timeoutMs, ChannelList* activeChannels);

    /// 更新 Channel 关心的事件
    void updateChannel(Channel* channel);

    /// 移除 Channel
    void removeChannel(Channel* channel);

    bool hasChannel(Channel* channel) const;

private:
    /// 填充就绪 Channel 到 activeChannels
    void fillActiveChannels(int numEvents,
                            ChannelList* activeChannels) const;

    /// 执行 epoll_ctl
    void update(int operation, Channel* channel);

    static const int kInitEventListSize = 16;

    int epollFd_;
    std::vector<struct epoll_event> events_;
    std::unordered_map<int, Channel*> channels_;
    EventLoop* ownerLoop_;
};

}  // namespace streaming
