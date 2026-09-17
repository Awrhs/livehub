#pragma once

#include "core/NonCopyable.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

class HttpServer;
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

private:
    StreamResolver resolver_;
};

}  // namespace streaming
