#include "webrtc/WhepHandler.hpp"
#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "core/Logger.hpp"
#include "rtmp/MediaStream.hpp"
#include "webrtc/Sdp.hpp"
#include "webrtc/WebRtcManager.hpp"
#include "webrtc/WebRtcTransport.hpp"

#include <sstream>

namespace streaming {

WhepHandler::WhepHandler(WebRtcManager* webrtc, StreamResolver resolver)
    : webrtc_(webrtc), resolver_(std::move(resolver)) {}

void WhepHandler::registerRoutes(HttpServer& server) {
    // WHEP：POST offer，返回 answer
    server.post("/whep/:stream",
                [this](const HttpRequest& req, HttpResponse& resp) {
                    std::string key = req.param("stream");
                    auto stream = resolver_ ? resolver_(key) : nullptr;
                    if (!stream) {
                        resp.setStatusCode(HttpStatusCode::k404NotFound);
                        resp.setContentType("application/json");
                        resp.setBody("{\"error\":\"stream not found\"}");
                        return;
                    }
                    auto transport = webrtc_->createSubscriber(key, stream);
                    if (!transport) {
                        resp.setStatusCode(
                            HttpStatusCode::k503ServiceUnavailable);
                        resp.setBody("no ports");
                        return;
                    }

                    SdpMessage offer;
                    offer.parse(req.body());
                    // 提取对端 fingerprint / setup 角色（actpass→active 服务端）
                    if (!offer.medias.empty()) {
                        transport->setRemoteFingerprint(
                            offer.medias[0].fingerprintHash);
                    }

                    // 构造 answer（sendonly 视频，H264 PT=96）
                    std::ostringstream o;
                    o << "v=0\r\n";
                    o << "o=- 0 0 IN IP4 0.0.0.0\r\n";
                    o << "s=streaming-cpp whep\r\n";
                    o << "t=0 0\r\n";
                    o << "a=group:BUNDLE 0\r\n";
                    o << "m=video " << transport->port()
                      << " UDP/TLS/RTP/SAVPF 96\r\n";
                    o << "c=IN IP4 0.0.0.0\r\n";
                    o << "a=rtcp:9 IN IP4 0.0.0.0\r\n";
                    o << "a=ice-ufrag:" << transport->ufrag() << "\r\n";
                    o << "a=ice-pwd:" << transport->pwd() << "\r\n";
                    o << "a=ice-options:trickle\r\n";
                    o << "a=fingerprint:" << webrtc_->localFingerprint()
                      << "\r\n";
                    o << "a=setup:passive\r\n";  // server 等 ClientHello
                    o << "a=mid:0\r\n";
                    o << "a=sendonly\r\n";
                    o << "a=rtcp-mux\r\n";
                    o << "a=rtpmap:96 H264/90000\r\n";
                    o << "a=fmtp:96 packetization-mode=1;"
                                         "profile-level-id=42E01F\r\n";
                    o << "a=rtcp-fb:96 nack\r\n";
                    o << "a=rtcp-fb:96 nack pli\r\n";
                    o << "a=ssrc:" << transport->ssrc()
                      << " cname:streaming\r\n";
                    o << transport->candidate() << "\r\n";
                    o << "a=end-of-candidates\r\n";

                    resp.setStatusCode(HttpStatusCode::k200Ok);
                    resp.setContentType("application/sdp");
                    resp.setBody(o.str());
                });
}

}  // namespace streaming
