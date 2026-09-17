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
 * WhipHandler: WHIP (WebRTC-HTTP Ingestion Protocol) 推流端。
 *
 * 路由  POST /whip/:stream
 *   请求体：客户端 SDP offer
 *   响应  ：201 Created + SDP answer（recvonly video m= 行）
 *
 * 每次请求创建一个 WebRtcIngest（ICE-lite + DTLS-SRTP 接收 H264 RTP）。
 */
class WhipHandler : NonCopyable {
public:
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string&,
                                                   bool createIfMissing)>;

    WhipHandler(WebRtcManager* webrtc, StreamResolver resolver);
    void registerRoutes(HttpServer& server);

private:
    WebRtcManager* webrtc_;
    StreamResolver resolver_;
};

}  // namespace streaming
