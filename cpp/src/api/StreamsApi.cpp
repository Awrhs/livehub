#include "api/StreamsApi.hpp"

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "api/Json.hpp"
#include "api/StreamStats.hpp"
#include "core/Timestamp.hpp"

namespace streaming {

StreamsApi::StreamsApi() = default;

static JsonValue toJson(const StreamInfo& s) {
    JsonValue o = JsonValue::object();
    o.set("key", JsonValue::str(s.key));
    o.set("publisher", JsonValue::boolean(s.hasPublisher));
    o.set("viewers", JsonValue::number(static_cast<double>(s.viewers)));
    o.set("video_bytes", JsonValue::number(static_cast<double>(s.videoBytes)));
    o.set("audio_bytes", JsonValue::number(static_cast<double>(s.audioBytes)));
    o.set("width", JsonValue::number(s.width));
    o.set("height", JsonValue::number(s.height));
    o.set("fps", JsonValue::number(s.fps));
    o.set("audio_sample_rate", JsonValue::number(s.audioSampleRate));
    o.set("audio_channels", JsonValue::number(s.audioChannels));
    o.set("video_bitrate", JsonValue::number(s.videoBitrate));
    o.set("audio_bitrate", JsonValue::number(s.audioBitrate));
    o.set("uptime_sec", JsonValue::number(static_cast<double>(s.uptimeSec)));
    return o;
}

void StreamsApi::attach(HttpServer& server) {
    server.router().get(
        "/api/streams",
        [this](const HttpRequest&, HttpResponse& resp) {
            std::vector<StreamInfo> streams =
                list_ ? list_() : std::vector<StreamInfo>{};
            JsonValue arr = JsonValue::array();
            for (const auto& s : streams) arr.append(toJson(s));
            JsonValue root = JsonValue::object();
            root.set("count", JsonValue::number(
                                  static_cast<double>(streams.size())));
            root.set("streams", arr);
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(root.dump());
        });

    server.router().get(
        "/api/streams/:key",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("key");
            auto st = StatsRegistry::instance().get(key);
            if (!st) {
                resp.setStatusCode(HttpStatusCode::k404NotFound);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"stream not found\"}");
                return;
            }
            StreamInfo s;
            s.key = key;
            s.hasPublisher = true;
            s.viewers = st->viewers();
            s.videoBytes = st->videoBytes();
            s.audioBytes = st->audioBytes();
            s.width = st->width();
            s.height = st->height();
            s.fps = st->fps();
            s.audioSampleRate = st->audioSampleRate();
            s.audioChannels = st->audioChannels();
            s.videoBitrate = st->videoBitrate();
            s.audioBitrate = st->audioBitrate();
            s.uptimeSec = Timestamp::now().secondsSinceEpoch() - st->startedSec();
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(toJson(s).dump());
        });

    auto handleDelete = [this](const HttpRequest& req, HttpResponse& resp) {
        std::string key = req.param("key");
        bool ok = kill_ ? kill_(key) : false;
        JsonValue o = JsonValue::object();
        o.set("key", JsonValue::str(key));
        o.set("killed", JsonValue::boolean(ok));
        resp.setStatusCode(ok ? HttpStatusCode::k200Ok
                              : HttpStatusCode::k404NotFound);
        resp.setContentType("application/json");
        resp.setBody(o.dump());
    };
    server.router().add(HttpMethod::DELETE, "/api/streams/:key", handleDelete);

    // ABR 主播放列表：/live/:stream_abr.m3u8 —— 因 Router 不支持后缀参数，
    // 这里用一条独立路由 /live/:stream/master.m3u8 更稳妥
    if (abrMaster_) {
        server.router().get(
            "/live/:stream/master.m3u8",
            [this](const HttpRequest& req, HttpResponse& resp) {
                std::string key = req.param("stream");
                std::string body = abrMaster_(key);
                resp.setStatusCode(HttpStatusCode::k200Ok);
                resp.setContentType("application/vnd.apple.mpegurl");
                resp.addHeader("Cache-Control", "max-age=2");
                resp.addHeader("Access-Control-Allow-Origin", "*");
                resp.setBody(body);
            });
    }
}

}  // namespace streaming
