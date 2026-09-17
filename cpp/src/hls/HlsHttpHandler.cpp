#include "hls/HlsHttpHandler.hpp"
#include "hls/HlsManager.hpp"
#include "hls/HlsSource.hpp"
#include "cache/StreamCache.hpp"
#include "api/HttpServer.hpp"
#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "core/Logger.hpp"
#include "protocol/HttpFlvHandler.hpp"
#include "rtmp/MediaStream.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <unistd.h>
#include <cstring>
#include <string>

namespace streaming {

HlsHttpHandler::HlsHttpHandler(HlsManager* manager, StreamCache* cache)
    : manager_(manager), cache_(cache) {}

static bool isSafeName(const std::string& s) {
    return !s.empty() && s.find("..") == std::string::npos &&
           s.find('/') == std::string::npos && s.find('\\') == std::string::npos;
}

static size_t parseRange(const std::string& rangeHeader, size_t fileSize,
                          size_t* outStart, size_t* outEnd) {
    // 形如 "bytes=START-END"
    if (rangeHeader.compare(0, 6, "bytes=") != 0) return 0;
    std::string spec = rangeHeader.substr(6);
    auto dash = spec.find('-');
    if (dash == std::string::npos) return 0;
    std::string s = spec.substr(0, dash);
    std::string e = spec.substr(dash + 1);
    size_t start = 0, end = fileSize - 1;
    if (!s.empty()) start = static_cast<size_t>(std::stoull(s));
    if (!e.empty()) end = static_cast<size_t>(std::stoull(e));
    if (start >= fileSize || start > end) return 0;
    if (end >= fileSize) end = fileSize - 1;
    *outStart = start;
    *outEnd = end;
    return end - start + 1;
}

bool HlsHttpHandler::servePlaylist(const std::string& stream,
                                   HttpResponse& resp) {
    if (!isSafeName(stream)) {
        resp.setStatusCode(HttpStatusCode::k400BadRequest);
        resp.setBody("{\"error\":\"bad stream name\"}");
        return true;
    }
    auto src = manager_->find(stream);
    if (!src) {
        resp.setStatusCode(HttpStatusCode::k404NotFound);
        resp.setContentType("application/json");
        resp.setBody("{\"error\":\"stream not found\"}");
        return true;
    }
    std::string body = src->playlist();
    resp.setStatusCode(HttpStatusCode::k200Ok);
    resp.setStatusMessage("OK");
    resp.setContentType("application/vnd.apple.mpegurl");
    resp.addHeader("Cache-Control", "max-age=2");
    resp.addHeader("Access-Control-Allow-Origin", "*");
    resp.setBody(body);
    return true;
}

void HlsHttpHandler::serveFlv(const std::string& key, HttpResponse& resp) {
    if (!flvResolver_) {
        resp.setStatusCode(HttpStatusCode::k404NotFound);
        resp.setBody("{\"error\":\"flv not enabled\"}");
        return;
    }
    HttpFlvHandler::serve(key, resp, flvResolver_);
}

void HlsHttpHandler::registerRoutes(HttpServer& server) {
    // 统一的 /live/:resource 单段路由：按后缀分发 .m3u8 / .flv，
    // 避免多个同形 /live/:x 路由按注册顺序互相遮蔽。
    server.router().get("/live/:resource",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string resource = req.param("resource");
            if (resource.size() >= 5 &&
                resource.compare(resource.size() - 4, 4, ".flv") == 0) {
                std::string key = resource.substr(0, resource.size() - 4);
                if (!isSafeName(key)) {
                    resp.setStatusCode(HttpStatusCode::k400BadRequest);
                    resp.setBody("{\"error\":\"bad stream name\"}");
                    return;
                }
                serveFlv(key, resp);
                return;
            }
            if (resource.size() >= 6 &&
                resource.compare(resource.size() - 5, 5, ".m3u8") == 0) {
                std::string stream = resource.substr(0, resource.size() - 5);
                servePlaylist(stream, resp);
                return;
            }
            resp.setStatusCode(HttpStatusCode::k404NotFound);
            resp.setContentType("application/json");
            resp.setBody("{\"error\":\"not found\"}");
        });

    // TS 分片：/live/test/12.ts
    server.router().get("/live/:stream/:segment",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string stream = req.param("stream");
            std::string segment = req.param("segment");
            if (!isSafeName(stream) || !isSafeName(segment)) {
                resp.setStatusCode(HttpStatusCode::k400BadRequest);
                resp.setBody("{\"error\":\"bad name\"}");
                return;
            }

            auto src = manager_->find(stream);
            if (!src) {
                resp.setStatusCode(HttpStatusCode::k404NotFound);
                resp.setBody("{\"error\":\"stream not found\"}");
                return;
            }

            std::string path = src->outputDir() + "/" + stream + "/" + segment;

            // 先查内存缓存
            if (cache_) {
                auto cached = cache_->get(path);
                if (cached) {
                    resp.setStatusCode(HttpStatusCode::k200Ok);
                    resp.setContentType("video/MP2T");
                    resp.addHeader("Access-Control-Allow-Origin", "*");
                    resp.setBody(*cached);
                    return;
                }
            }

            int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) {
                resp.setStatusCode(HttpStatusCode::k404NotFound);
                resp.setBody("{\"error\":\"segment not found\"}");
                return;
            }
            struct stat st{};
            if (::fstat(fd, &st) < 0) {
                ::close(fd);
                resp.setStatusCode(HttpStatusCode::k500InternalServerError);
                resp.setBody("{\"error\":\"stat failed\"}");
                return;
            }
            size_t fileSize = static_cast<size_t>(st.st_size);

            resp.setContentType("video/MP2T");
            resp.addHeader("Access-Control-Allow-Origin", "*");

            // Range 支持 -> 206
            std::string rangeHeader = req.header("Range");
            size_t start = 0, end = 0;
            if (!rangeHeader.empty() &&
                parseRange(rangeHeader, fileSize, &start, &end) > 0) {
                resp.setRangeBody(fd, fileSize, start, end, "video/MP2T");
            } else {
                resp.setFileBody(fd, fileSize, "video/MP2T");
            }
        });
}

}  // namespace streaming
