#pragma once

#include "core/NonCopyable.hpp"
#include "net/UdpServer.hpp"
#include "rtmp/MediaStream.hpp"
#include "srt/SrtSession.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace streaming {

class EventLoop;

/**
 * SrtServer: 在 UDP 端口上承载 SRT 协议（listener 模式）。
 *
 * - 推流：ffmpeg -i in.ts -c copy srt://host:port?streamid=#!::r=live/key
 *   收到 caller 握手后创建一条 SrtSession，TS 经 TsDemuxer 重建 MediaStream。
 * - 拉流：按 streamid 找到本地 MediaStream，attachPlayStream 用 TS 发出。
 *
 * 每对 (ip,port) 维护一个 SrtSession；空闲超时回收。
 */
class SrtServer : NonCopyable {
public:
    /// 给定 stream key 返回/创建一个 MediaStream（ingest 用）
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string& key)>;
    /// 拉流：按 key 找已存在的 MediaStream
    using PlayResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string& key)>;
    /// ingest 推流结束时回调（清理业务侧流）
    using UnpublishHandler =
        std::function<void(const std::string& key)>;

    SrtServer(EventLoop* loop, uint16_t port,
              const std::string& ip = "0.0.0.0");
    ~SrtServer();

    void start();

    void setStreamResolver(StreamResolver r) { resolver_ = std::move(r); }
    void setPlayResolver(PlayResolver r) { playResolver_ = std::move(r); }
    void setUnpublishHandler(UnpublishHandler h) { onUnpublish_ = std::move(h); }

    /// 主动以 caller 身份向对端推一路流（SRT 转发）
    std::shared_ptr<SrtSession> pushTo(const std::string& host, uint16_t port,
                                       const std::string& streamId,
                                       const std::shared_ptr<MediaStream>& src);

    size_t sessionCount() const;

private:
    void onMessage(const uint8_t* data, size_t len, const sockaddr_in& peer,
                   Timestamp t);
    std::shared_ptr<SrtSession> getOrCreate(const sockaddr_in& peer);
    static std::string peerKey(const sockaddr_in& peer);

    EventLoop* loop_;
    std::unique_ptr<UdpServer> udp_;
    std::atomic<uint32_t> nextSocketId_{1};

    StreamResolver resolver_;
    PlayResolver playResolver_;
    UnpublishHandler onUnpublish_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<SrtSession>> sessions_;
};

}  // namespace streaming
