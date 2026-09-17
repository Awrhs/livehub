#include "middleware/AccessLog.hpp"

#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "metrics/Metrics.hpp"

#include <cstdio>
#include <string>

namespace streaming {

namespace {
const char* methodName(HttpMethod m) {
    switch (m) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE: return "DELETE";
        case HttpMethod::HEAD: return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}
}  // namespace

std::string formatAccessLogJson(const std::string& ts,
                                const std::string& traceId,
                                const std::string& ip,
                                const std::string& method,
                                const std::string& path, int status,
                                size_t bytes, double elapsedMs,
                                const std::string& ua) {
    char head[256];
    std::snprintf(head, sizeof(head),
                  "{\"ts\":\"%s\",\"trace_id\":\"%s\",\"ip\":\"%s\","
                  "\"method\":\"%s\",\"path\":\"",
                  ts.c_str(), Logger::jsonEscape(traceId).c_str(),
                  Logger::jsonEscape(ip).c_str(),
                  Logger::jsonEscape(method).c_str());
    char tail[128];
    std::snprintf(tail, sizeof(tail),
                  "\",\"status\":%d,\"bytes\":%zu,\"elapsed_ms\":%.3f,\"ua\":\"",
                  status, bytes, elapsedMs);
    std::string line;
    line.reserve(256 + path.size() + ua.size());
    line += head;
    line += Logger::jsonEscape(path);
    line += tail;
    line += Logger::jsonEscape(ua);
    line += "\"}";
    return line;
}

void AccessLog::operator()(const HttpRequest& req,
                            const HttpResponse& resp,
                            double elapsedSeconds) {
    static auto* reqCounter = Metrics::instance().counter(
        "http_requests_total", "Total HTTP requests");
    static auto* reqDuration = Metrics::instance().histogram(
        "http_duration_seconds", "HTTP request latency in seconds");

    int status = static_cast<int>(resp.statusCode());
    reqCounter->inc();
    reqDuration->observe(elapsedSeconds);
    if (status >= 400) {
        static auto* errCounter = Metrics::instance().counter(
            "http_errors_total", "HTTP 4xx/5xx responses");
        errCounter->inc();
    }

    double ms = elapsedSeconds * 1000.0;
    if (Logger::instance().jsonMode()) {
        // JSON 模式：自行组装完整 JSON 行（含 trace_id），直写不走 LOG 宏，
        // 避免被 Logger 再包一层 envelope。
        std::string line = formatAccessLogJson(
            Timestamp::now().toFormattedString(true), logging::traceId(),
            req.clientIp, methodName(req.method()), req.path(), status,
            resp.body().size(), ms, req.header("User-Agent"));
        Logger::instance().logRaw(line.data(), line.size());
    } else {
        // 默认单行；[trace=xx] 由 LOG_* 自动追加
        LOG_INFO("%s \"%s %s\" %d %zu %.2fms \"%s\"",
                 req.clientIp.c_str(), methodName(req.method()),
                 req.path().c_str(), status, resp.body().size(), ms,
                 req.header("User-Agent").c_str());
    }

    // 请求生命周期终点：清空线程本地 trace id，避免沾染线程上
    // 后续非请求日志。
    logging::setTraceId("");
}

}  // namespace streaming
