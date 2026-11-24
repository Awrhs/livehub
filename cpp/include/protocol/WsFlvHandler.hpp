#pragma once

#include "core/NonCopyable.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

class TcpConnection;

class HttpServer;
class HttpRequest;
class MediaStream;

/**
 * WsFlvHandler: WebSocket-FLV 直播（低延迟 Web 播放）。
 *
 * 作为 HttpServer 的协议升级回调注册：
 *   路由  GET(s) /live/:stream.flv  Upgrade: websocket
 *   -> 101 Switching Protocols，Sec-WebSocket-Accept
 *   -> binary 帧封装 FLV tag，持续推送
 *
 * FLV 复用 FlvMuxer：先发 FLV header + script tag + sequence header/GOP，
 * 再订阅 MediaStream 实时转发。客户端发 ping 自动回 pong，发 close 则断开。
 */
class WsFlvHandler : NonCopyable {
public:
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string&)>;

    WsFlvHandler(StreamResolver resolver);

    /// 注册为 HttpServer 的升级回调（内部检查路径）
    void attach(HttpServer& server);

    /// 播放鉴权（与 HTTP-FLV/HLS 一致）；未设置则不鉴权
    using PlayAuth = std::function<bool(const HttpRequest& req, const std::string& key,
                                        std::string* reason)>;
    void setPlayAuth(PlayAuth a) { playAuth_ = std::move(a); }

    /// 异步播放鉴权（含 on_play webhook）：设置后替代 PlayAuth，等待期间不阻塞 IO 线程
    using AsyncPlayAuth = std::function<void(const std::string& host, const std::string& rawKey,
                                             std::function<void(bool ok, const std::string& reason)> done)>;
    void setAsyncPlayAuth(AsyncPlayAuth a) { asyncPlayAuth_ = std::move(a); }

private:
    /// 鉴权通过后：完成握手并开始推送（在连接所属 IO 线程调用）
    void accept(const std::shared_ptr<TcpConnection>& conn, const std::string& key,
                const std::string& wsKey);

    StreamResolver resolver_;
    PlayAuth playAuth_;
    AsyncPlayAuth asyncPlayAuth_;
};

}  // namespace streaming
