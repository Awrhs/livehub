#pragma once

#include "net/TcpServer.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

class MediaStream;

/**
 * RtspServer: RTSP 直播服务端（RFC 2326 子集）。
 *
 * 监听 TCP 554（默认），从 URL rtsp://host:port/live/:stream 解析流名，
 * 经 StreamResolver 找到 MediaStream 后创建 RtspSession，采用 RTP over
 * TCP 交织模式下行。
 */
class RtspServer {
public:
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string&)>;

    RtspServer(EventLoop* loop, uint16_t port = 8554);

    void setStreamResolver(StreamResolver r) { resolver_ = std::move(r); }
    void setThreadNum(int n) { server_.setThreadNum(n); }
    void start();
    void stop() { server_.stop(); }

    uint16_t port() const { return port_; }

private:
    void onConnection(const TcpConnectionPtr& conn);
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp);

    [[maybe_unused]] EventLoop* loop_;
    uint16_t port_;
    TcpServer server_;
    StreamResolver resolver_;
};

}  // namespace streaming
