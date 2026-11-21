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

// 解析 "bytes=START-END" / "bytes=START-" / "bytes=-SUFFIX"。非法返回 0。
// （原实现用 std::stoull，畸形输入抛异常可打崩进程；且后缀范围语义错误）
static bool parseU64(const std::string& s, size_t* out) {
    if (s.empty() || s.size() > 19) return false;
    size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    *out = v;
    return true;
}

static size_t parseRange(const std::string& rangeHeader, size_t fileSize,
                         size_t* outStart, size_t* outEnd) {
    if (fileSize == 0 || rangeHeader.compare(0, 6, "bytes=") != 0) return 0;
    std::string spec = rangeHeader.substr(6);
    if (spec.find(',') != std::string::npos) return 0;  // 不支持多段
    auto dash = spec.find('-');
    if (dash == std::string::npos) return 0;
    std::string s = spec.substr(0, dash);
    std::string e = spec.substr(dash + 1);
    size_t start = 0, end = fileSize - 1, v = 0;
    if (s.empty()) {
        if (!parseU64(e, &v) || v == 0) return 0;  // 后缀：最后 v 字节
        start = v >= fileSize ? 0 : fileSize - v;
    } else {
        if (!parseU64(s, &start)) return 0;
        if (!e.empty()) {
            if (!parseU64(e, &end)) return 0;
        }
    }
    if (start >= fileSize || start > end) return 0;
    if (end >= fileSize) end = fileSize - 1;
    *outStart = start;
    *outEnd = end;
    return end - start + 1;
}

bool HlsHttpHandler::checkAuth(const HttpRequest& req, const std::string& key,
                               bool hlsPoll, HttpResponse& resp) {
    if (!playAuth_) return true;
    std::string reason;
    if (playAuth_(req, key, hlsPoll, &reason)) return true;
    resp.setStatusCode(HttpStatusCode::k403Forbidden);
    resp.setContentType("application/json");
    resp.setBody("{\"error\":\"" + (reason.empty() ? std::string("forbidden") : reason) + "\"}");
    return false;
}

bool HlsHttpHandler::servePlaylist(const std::string& stream,
                                   HttpResponse& resp,
                                   const std::string& token) {
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
    if (!token.empty()) {
        // 播放器请求分片时不会自动带上 m3u8 的查询串：为每个分片 URI 追加 token
        std::string out;
        out.reserve(body.size() + 64);
        size_t pos = 0;
        while (pos < body.size()) {
            size_t nl = body.find('\n', pos);
            std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            out += line;
            if (!line.empty() && line[0] != '#') out += "?token=" + token;
            out += '\n';
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        body.swap(out);
    }
    resp.setStatusCode(HttpStatusCode::k200Ok);
    resp.setStatusMessage("OK");
    resp.setContentType("application/vnd.apple.mpegurl");
    resp.addHeader("Cache-Control", "no-cache");
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
                if (!checkAuth(req, key, false, resp)) return;
                serveFlv(key, resp);
                return;
            }
            if (resource.size() >= 6 &&
                resource.compare(resource.size() - 5, 5, ".m3u8") == 0) {
                std::string stream = resource.substr(0, resource.size() - 5);
                if (!isSafeName(stream)) {
                    resp.setStatusCode(HttpStatusCode::k400BadRequest);
                    resp.setBody("{\"error\":\"bad stream name\"}");
                    return;
                }
                if (!checkAuth(req, stream, true, resp)) return;
                servePlaylist(stream, resp, req.param("token"));
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

            if (!checkAuth(req, stream, true, resp)) return;
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
