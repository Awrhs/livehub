#include "webrtc/WhipHandler.hpp"
#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "core/Logger.hpp"
#include "rtmp/MediaStream.hpp"
#include "webrtc/Sdp.hpp"
#include "webrtc/SdpAnswer.hpp"
#include "webrtc/WebRtcUtil.hpp"
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
                    auto fail = [&resp](HttpStatusCode code, const std::string& msg) {
                        resp.setStatusCode(code);
                        resp.setContentType("application/json");
                        resp.setBody("{\"error\":\"" + msg + "\"}");
                    };
                    std::string reason;
                    if (auth_ && !auth_(req, key, &reason))
                        return fail(HttpStatusCode::k403Forbidden, reason.empty() ? "forbidden" : reason);
                    // WHIP 推流：流名已有推流者时由 resolver 返回空
                    auto stream = resolver_ ? resolver_(key, true) : nullptr;
                    if (!stream) return fail(HttpStatusCode::k409Conflict, "stream busy or not allowed");
                    auto ingest = webrtc_->createIngest(key, stream);
                    if (!ingest) return fail(HttpStatusCode::k503ServiceUnavailable, "no ports");

                    SdpMessage offer;
                    offer.parse(req.body());
                    if (!offer.medias.empty())
                        ingest->setRemoteFingerprint(offer.medias[0].fingerprintHash);

                    SdpAnswerParams p;
                    p.sendVideo = false;
                    p.port = ingest->port();
                    p.ufrag = ingest->ufrag();
                    p.pwd = ingest->pwd();
                    p.fingerprint = webrtc_->localFingerprint();
                    p.candidateLine = webrtcCandidateLine(candidateIp_, ingest->port());
                    auto ans = buildSdpAnswer(req.body(), p);
                    if (!ans.ok) return fail(HttpStatusCode::k400BadRequest, ans.error);
                    stream->setHasPublisher(true);

                    resp.setStatusCode(HttpStatusCode::k201Created);
                    resp.setContentType("application/sdp");
                    resp.addHeader("Location", "/whip/" + key);
                    resp.setBody(ans.sdp);
                });
}

}  // namespace streaming
