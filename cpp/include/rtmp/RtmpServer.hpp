#pragma once

#include "config/Vhost.hpp"
#include "net/TcpServer.hpp"
#include "rtmp/RtmpConnection.hpp"
#include "rtmp/MediaStream.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace streaming {

class EventLoop;
class RtmpConnection;

/**
 * RtmpServer: RTMP 服务器（默认 1935 端口）。
 *
 * 基于 TcpServer（mainReactor + subReactor），每连接创建一个 RtmpConnection。
 * 维护 streamKey -> MediaStream 注册表，供播放端/桥接器查找。
 */
class RtmpServer {
public:
    using PublishHandler =
        std::function<void(const std::shared_ptr<MediaStream>& stream)>;
    using PlayHandler = std::function<void(
        const std::shared_ptr<RtmpConnection>& conn,
        const std::string& app, const std::string& streamKey)>;
    using UnpublishHandler =
        std::function<void(const std::string& app, const std::string& streamKey)>;
    /// 异步鉴权回调：host 为 tcUrl 解析出的 vhost，publish=true 推流；
    /// 结果经 done(ok, reason) 返回（可在任意线程调用）。
    using AuthHandler = RtmpConnection::AuthCallback;

    RtmpServer(EventLoop* loop, uint16_t port = 1935,
               std::string app = "live");
    ~RtmpServer();

    void setThreadNum(int n) { server_.setThreadNum(n); }
    void setIdleTimeout(int seconds) { server_.setIdleTimeout(seconds); }
    void setConnectionLimits(int total, int perIp) { server_.setConnectionLimits(total, perIp); }
    void stop() { server_.stop(); }
    void setPublishHandler(PublishHandler h) { onPublish_ = std::move(h); }
    void setPlayHandler(PlayHandler h) { onPlay_ = std::move(h); }
    void setUnpublishHandler(UnpublishHandler h) { onUnpublish_ = std::move(h); }
    void setAuthHandler(AuthHandler h) { onAuth_ = std::move(h); }
    /// 设置 vhost 解析器（按 host 返回 Vhost），用于连接关闭时回收配额。
    using VhostResolver = std::function<VhostPtr(const std::string& host)>;
    void setVhostResolver(VhostResolver r) { vhostResolver_ = std::move(r); }

    void start();

    /// 查找已发布的流
    std::shared_ptr<MediaStream> findStream(const std::string& key) const;

    /// 查找或创建一路流（供 WHIP 上行等非 RTMP 发布源注册）
    std::shared_ptr<MediaStream> getOrCreateStream(const std::string& key);

    /// 列出所有流 key（供统计/管理 API）
    std::vector<std::string> listKeys() const;

    /// 移除流；owner 非空时仅当当前登记的流就是 owner 才移除
    void removeStream(const std::string& key,
                      const std::shared_ptr<MediaStream>& owner = nullptr);
    size_t streamCount() const;

private:
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp t);

    TcpServer server_;
    std::string app_;

    PublishHandler onPublish_;
    PlayHandler onPlay_;
    UnpublishHandler onUnpublish_;
    AuthHandler onAuth_;
    VhostResolver vhostResolver_;

    VhostPtr resolveVhost(const std::string& host) const {
        return vhostResolver_ ? vhostResolver_(host) : nullptr;
    }

    mutable std::mutex streamsMu_;
    std::unordered_map<std::string, std::shared_ptr<MediaStream>> streams_;
};

}  // namespace streaming
