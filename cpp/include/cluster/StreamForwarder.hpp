#pragma once

#include "core/NonCopyable.hpp"
#include "rtmp/MediaStream.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

class EventLoop;
class TcpClient;
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
class Buffer;

/**
 * StreamForwarder: 把一路本地 MediaStream 主动转推（RTMP push）到多个上游。
 *
 * 典型用途：一路推流进来后，同时转发到多个直播平台/源站
 *   rtmp://host1/app/key, rtmp://host2/app/key ...
 *
 * 每个上游是一个异步 RTMP 推流会话（非阻塞，运行在传入的 EventLoop 上）：
 *   1. TCP 连接 + RTMP 简单握手（C0/C1/C2）
 *   2. AMF0 connect / releaseStream / FCPublish / createStream / publish
 *   3. 转发 @setDataFrame/onMetaData 与 type8/type9 音视频消息
 *   4. 断线按指数退避自动重连
 *
 * 简历点：多目的地转推、RTMP 客户端协议、断线重连、背压队列。
 */
class StreamForwarder : NonCopyable {
public:
    StreamForwarder(EventLoop* loop, std::shared_ptr<MediaStream> stream);
    ~StreamForwarder();

    /// 增加一个转推目的地（rtmp://host[:port]/app/key）。返回该目的地 id。
    uint64_t addTarget(const std::string& url);

    /// 移除某个目的地
    void removeTarget(uint64_t id);

    /// 开始订阅源流并转发
    void start();
    void stop();

    size_t targetCount() const;

    struct Status {
        uint64_t id;
        std::string url;
        bool connected;
        uint64_t reconnects;
        uint64_t bytesSent;
    };
    std::vector<Status> status() const;

private:
    struct Target;
    void onPacket(const MediaPacket& pkt);

    EventLoop* loop_;
    std::shared_ptr<MediaStream> stream_;
    MediaStream::SubscriberId subId_ = 0;
    std::atomic<bool> running_{false};

    mutable std::mutex mu_;
    std::map<uint64_t, std::shared_ptr<Target>> targets_;
    std::atomic<uint64_t> nextId_{1};
};

}  // namespace streaming
