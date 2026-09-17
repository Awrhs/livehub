#include "dvr/DvrHandler.hpp"

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "api/Json.hpp"
#include "core/Logger.hpp"
#include "dvr/DvrManager.hpp"
#include "dvr/Fmp4Recorder.hpp"
#include "rtmp/MediaStream.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace streaming {

namespace {

bool isSafeName(const std::string& s) {
    return !s.empty() && s.find("..") == std::string::npos &&
           s.find('/') == std::string::npos && s.find('\\') == std::string::npos;
}

bool stripSuffix(const std::string& s, const std::string& suffix,
                 std::string& out) {
    if (s.size() <= suffix.size()) return false;
    if (s.compare(s.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    out = s.substr(0, s.size() - suffix.size());
    return true;
}

void jsonError(HttpResponse& resp, HttpStatusCode code, const std::string& msg) {
    resp.setStatusCode(code);
    resp.setContentType("application/json");
    JsonValue o = JsonValue::object();
    o.set("error", JsonValue::str(msg));
    resp.setBody(o.dump());
}

size_t parseRange(const std::string& rangeHeader, size_t fileSize,
                  size_t* start, size_t* end) {
    if (rangeHeader.compare(0, 6, "bytes=") != 0) return 0;
    std::string spec = rangeHeader.substr(6);
    auto dash = spec.find('-');
    if (dash == std::string::npos) return 0;
    std::string s = spec.substr(0, dash);
    std::string e = spec.substr(dash + 1);
    size_t st = 0, en = fileSize - 1;
    if (!s.empty()) st = static_cast<size_t>(std::stoull(s));
    if (!e.empty()) en = static_cast<size_t>(std::stoull(e));
    if (st >= fileSize || st > en) return 0;
    if (en >= fileSize) en = fileSize - 1;
    *start = st;
    *end = en;
    return en - st + 1;
}

}  // namespace

DvrHandler::DvrHandler(DvrManager* dvr, StreamResolver resolver)
    : dvr_(dvr), resolver_(std::move(resolver)) {}

void DvrHandler::attach(HttpServer& server) {
    // ---- 录像控制 ----
    server.router().post(
        "/api/record/:stream/start",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("stream");
            if (!isSafeName(key)) {
                jsonError(resp, HttpStatusCode::k400BadRequest, "bad name");
                return;
            }
            auto ms = resolver_ ? resolver_(key) : nullptr;
            if (!ms) {
                jsonError(resp, HttpStatusCode::k404NotFound,
                          "stream not found");
                return;
            }
            std::string path = dvr_->startRecord(ms);
            JsonValue o = JsonValue::object();
            o.set("stream", JsonValue::str(key));
            o.set("recording", JsonValue::boolean(true));
            o.set("path", JsonValue::str(path));
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(o.dump());
        });

    server.router().post(
        "/api/record/:stream/stop",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("stream");
            if (!isSafeName(key)) {
                jsonError(resp, HttpStatusCode::k400BadRequest, "bad name");
                return;
            }
            bool ok = dvr_->stopRecord(key);
            JsonValue o = JsonValue::object();
            o.set("stream", JsonValue::str(key));
            o.set("recording", JsonValue::boolean(false));
            o.set("stopped", JsonValue::boolean(ok));
            resp.setStatusCode(ok ? HttpStatusCode::k200Ok
                                  : HttpStatusCode::k404NotFound);
            resp.setContentType("application/json");
            resp.setBody(o.dump());
        });

    server.router().get(
        "/api/record/:stream/status",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("stream");
            bool rec = dvr_->isRecording(key);
            JsonValue o = JsonValue::object();
            o.set("stream", JsonValue::str(key));
            o.set("recording", JsonValue::boolean(rec));
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(o.dump());
        });

    // ---- 录像下载（VOD MP4，支持 Range） ----
    server.router().get(
        "/dvr/recordings/:file",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string file = req.param("file");
            std::string key;
            if (!stripSuffix(file, ".mp4", key) || !isSafeName(key)) {
                jsonError(resp, HttpStatusCode::k400BadRequest, "bad name");
                return;
            }
            std::string path = dvr_->recordPath(key);
            int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) {
                jsonError(resp, HttpStatusCode::k404NotFound,
                          "recording not found");
                return;
            }
            struct stat st{};
            if (::fstat(fd, &st) < 0) {
                ::close(fd);
                jsonError(resp, HttpStatusCode::k500InternalServerError,
                          "stat failed");
                return;
            }
            resp.addHeader("Access-Control-Allow-Origin", "*");
            size_t fileSize = static_cast<size_t>(st.st_size);
            std::string range = req.header("Range");
            size_t st2 = 0, en = 0;
            if (!range.empty() &&
                parseRange(range, fileSize, &st2, &en) > 0) {
                resp.setRangeBody(fd, fileSize, st2, en, "video/mp4");
            } else {
                resp.setFileBody(fd, fileSize, "video/mp4");
            }
        });

    // ---- 时移分片（静态文件：init.mp4 / seg-N.m4s） ----
    server.router().get(
        "/timeshift/:stream/:file",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("stream");
            std::string file = req.param("file");
            if (!isSafeName(key) || !isSafeName(file)) {
                jsonError(resp, HttpStatusCode::k400BadRequest, "bad name");
                return;
            }
            // 确保时移录制器已启动（若流存在）
            if (resolver_) {
                if (auto ms = resolver_(key)) dvr_->enableTimeshift(ms);
            }
            std::string path = dvr_->timeshiftDir(key) + "/" + file;
            int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) {
                jsonError(resp, HttpStatusCode::k404NotFound,
                          "segment not found");
                return;
            }
            struct stat st{};
            if (::fstat(fd, &st) < 0) {
                ::close(fd);
                jsonError(resp, HttpStatusCode::k500InternalServerError,
                          "stat failed");
                return;
            }
            std::string ct = (file == "init.mp4")
                                 ? "video/mp4"
                                 : "video/iso.segment";
            resp.addHeader("Access-Control-Allow-Origin", "*");
            resp.addHeader("Cache-Control", "max-age=2");
            resp.setFileBody(fd, static_cast<size_t>(st.st_size), ct);
        });
}

}  // namespace streaming
