#include "webrtc/WhepHandler.hpp"
#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "core/Logger.hpp"
#include "rtmp/MediaStream.hpp"
#include "webrtc/AacToOpus.hpp"
#include "webrtc/Sdp.hpp"
#include "webrtc/SdpAnswer.hpp"
#include "webrtc/WebRtcUtil.hpp"
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
                    auto fail = [&resp](HttpStatusCode code, const std::string& msg) {
                        resp.setStatusCode(code);
                        resp.setContentType("application/json");
                        resp.setBody("{\"error\":\"" + msg + "\"}");
                    };
                    std::string reason;
                    if (auth_ && !auth_(req, key, &reason))
                        return fail(HttpStatusCode::k403Forbidden, reason.empty() ? "forbidden" : reason);
                    auto stream = resolver_ ? resolver_(key) : nullptr;
                    if (!stream) return fail(HttpStatusCode::k404NotFound, "stream not found");

                    // 先校验 offer，再分配传输资源
                    SdpAnswerParams p;
                    p.sendVideo = true;
                    p.sendAudio = AacToOpus::available();
                    auto probe = buildSdpAnswer(req.body(), p);
                    if (!probe.ok) return fail(HttpStatusCode::k400BadRequest, probe.error);

                    auto transport = webrtc_->createSubscriber(key, stream);
                    if (!transport) return fail(HttpStatusCode::k503ServiceUnavailable, "no ports");
                    // 对端 DTLS 证书指纹（按 sha-256 选取；offer 可能同时含多种算法）
                    transport->setRemoteFingerprint(sdpFingerprint(req.body(), "sha-256"));

                    p.port = transport->port();
                    p.ufrag = transport->ufrag();
                    p.pwd = transport->pwd();
                    p.fingerprint = webrtc_->localFingerprint();
                    p.candidateLine = webrtcCandidateLine(candidateIp_, transport->port());
                    p.ssrc = transport->ssrc();
                    p.audioSsrc = transport->audioSsrc();
                    auto ans = buildSdpAnswer(req.body(), p);
                    transport->setVideoPayloadType(static_cast<uint8_t>(ans.videoPayloadType));
                    transport->setAudioPayloadType(ans.audioPayloadType);

                    resp.setStatusCode(HttpStatusCode::k201Created);
                    resp.setContentType("application/sdp");
                    resp.addHeader("Location", "/whep/" + key);
                    resp.setBody(ans.sdp);
                });
}

}  // namespace streaming
