#pragma once

#include "core/NonCopyable.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

class HttpServer;
class HttpRequest;
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
    /// 推/拉流鉴权：key 为流名，token 来自 Authorization: Bearer 或 ?token=
    using AuthFn = std::function<bool(const HttpRequest& req, const std::string& key,
                                      std::string* reason)>;
    void setAuth(AuthFn f) { auth_ = std::move(f); }
    /// ICE host 候选地址（为空则自动探测首个非回环 IPv4）
    void setCandidateIp(std::string ip) { candidateIp_ = std::move(ip); }


private:
    WebRtcManager* webrtc_;
    StreamResolver resolver_;
    AuthFn auth_;
    std::string candidateIp_;
};

}  // namespace streaming
