#include "webrtc/WhipHandler.hpp"
#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "core/Logger.hpp"
#include "rtmp/MediaStream.hpp"
#include "webrtc/Sdp.hpp"
#include "webrtc/WebRtcManager.hpp"
#include "webrtc/WebRtcIngest.hpp"

#include <sstream>

namespace streaming {

WhipHandler::WhipHandler(WebRtcManager* webrtc, StreamResolver resolver)
    : webrtc_(webrtc), resolver_(std::move(resolver)) {}

void WhipHandler::registerRoutes(HttpServer& server) {
    server.post("/whip/:stream",
                [this](const HttpRequest& req, HttpResponse& resp) {
                    std::string key = req.param("stream");
                    // WHIP 推流：若流不存在则创建（发布者）
                    auto stream =
                        resolver_ ? resolver_(key, true) : nullptr;
                    if (!stream) {
                        resp.setStatusCode(HttpStatusCode::k404NotFound);
                        resp.setContentType("application/json");
                        resp.setBody("{\"error\":\"cannot create stream\"}");
                        return;
                    }
                    auto ingest = webrtc_->createIngest(key, stream);
                    if (!ingest) {
                        resp.setStatusCode(
                            HttpStatusCode::k503ServiceUnavailable);
                        resp.setBody("no ports");
                        return;
                    }
                    SdpMessage offer;
                    offer.parse(req.body());
                    if (!offer.medias.empty()) {
                        ingest->setRemoteFingerprint(
                            offer.medias[0].fingerprintHash);
                    }
                    stream->setHasPublisher(true);

                    std::ostringstream o;
                    o << "v=0\r\n";
                    o << "o=- 0 0 IN IP4 0.0.0.0\r\n";
                    o << "s=streaming-cpp whip\r\n";
                    o << "t=0 0\r\n";
                    o << "m=video " << ingest->port()
                      << " UDP/TLS/RTP/SAVPF 96\r\n";
                    o << "c=IN IP4 0.0.0.0\r\n";
                    o << "a=ice-ufrag:" << ingest->ufrag() << "\r\n";
                    o << "a=ice-pwd:" << ingest->pwd() << "\r\n";
                    o << "a=ice-options:trickle\r\n";
                    o << "a=fingerprint:" << webrtc_->localFingerprint()
                      << "\r\n";
                    o << "a=setup:passive\r\n";
                    o << "a=recvonly\r\n";
                    o << "a=rtcp-mux\r\n";
                    o << "a=rtpmap:96 H264/90000\r\n";
                    o << "a=fmtp:96 packetization-mode=1\r\n";
                    o << ingest->candidate() << "\r\n";
                    o << "a=end-of-candidates\r\n";

                    resp.setStatusCode(HttpStatusCode::k201Created);
                    resp.setContentType("application/sdp");
                    resp.setBody(o.str());
                });
}

}  // namespace streaming
