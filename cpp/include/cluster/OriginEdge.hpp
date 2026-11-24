#pragma once

#include "core/NonCopyable.hpp"
#include "net/TcpClient.hpp"
#include "protocol/FlvDemuxer.hpp"
#include "rtmp/MediaStream.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

class EventLoop;

/**
 * OriginEdge: 边缘节点回源。
 *
 * 当本地未命中某路流时，向源站发起 HTTP-FLV pull：
 *   GET /live/<key>.flv HTTP/1.1
 * 读取响应头后，用 FlvDemuxer 解析 FLV tag，重建本地 MediaStream，
 * 再分发给本节点观众；拉流连接断开时自动销毁。
 *
 * 每路被拉的流一个 TcpClient + 一个 FlvDemuxer。
 */
class OriginEdge : NonCopyable {
public:
    OriginEdge(EventLoop* loop, std::string originHost,
               uint16_t originPort = 8080, std::string app = "live");
    ~OriginEdge();

    /// 获取（或开始回源）一路流；返回本地 MediaStream。
    std::shared_ptr<MediaStream> getOrPull(const std::string& key);

    /// 释放一路流（无观众时）
    void release(const std::string& key);

    size_t activePulls() const;

    /// 当前回源中的流名
    std::vector<std::string> keys() const;
    /// 回源流（不增加引用）；不存在返回 nullptr
    std::shared_ptr<MediaStream> find(const std::string& key) const;
    /// 回源已持续的毫秒数（新回源在观众订阅前不应被回收）
    int64_t ageMs(const std::string& key) const;
    /// 立即停止回源（无观众时由空闲检查调用）
    void stop(const std::string& key);

private:
    struct PullSession;

    EventLoop* loop_;
    std::string originHost_;
    uint16_t originPort_;
    std::string app_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<PullSession>> sessions_;
};

}  // namespace streaming
