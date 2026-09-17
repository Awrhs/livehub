#pragma once

#include "core/NonCopyable.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

class HttpServer;
class MediaStream;
class WebRtcManager;

/**
 * WhepHandler: WHEP（WebRTC-HTTP Egress Protocol, draft）观看端。
 *
 * 路由  POST /whep/:stream
 *   请求体：客户端 SDP offer
 *   响应  ：201 Created + SDP answer（含 ICE ufrag/pwd、DTLS fingerprint、
 *          candidate、sendonly video m= 行）
 *
 * 每次请求创建一个 WebRtcTransport（ICE-lite + DTLS-SRTP 下行）。
 */
class WhepHandler : NonCopyable {
public:
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string&)>;

    WhepHandler(WebRtcManager* webrtc, StreamResolver resolver);

    void registerRoutes(HttpServer& server);

private:
    WebRtcManager* webrtc_;
    StreamResolver resolver_;
};

}  // namespace streaming
