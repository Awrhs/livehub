#pragma once

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "core/NonCopyable.hpp"

#include <functional>
#include <string>

namespace streaming {

/**
 * AccessLog: 访问日志 + Metrics 埋点。
 *
 * 挂到 HttpServer::setAfterRequest，记录 method/path/status/bytes/耗时，
 * 同时累加 http_requests_total、观察 http_duration_seconds。
 *
 * 双格式（跟随 Logger::jsonMode()）：
 * - 默认单行（便于人读/Grep）：
 *   <ip> "<method> <path>" <status> <bytes> <elapsed_ms>ms "<ua>" [trace=xx]
 *   （trace 由 LOG_* 自动追加）
 * - JSON 模式：完整 JSON 行（含 trace_id），经 Logger::logRaw 直写，
 *   便于 Loki/ELK 采集。
 *
 * 请求结束时清空线程本地 trace id（本回调是每个请求的最后一站）。
 */
class AccessLog : NonCopyable {
public:
    AccessLog() = default;

    void operator()(const HttpRequest& req, const HttpResponse& resp,
                    double elapsedSeconds);
};

/// 组装一条 JSON 访问日志行（独立函数便于单测）。
/// 字段：ts/trace_id/ip/method/path/status/bytes/elapsed_ms/ua
std::string formatAccessLogJson(const std::string& ts,
                                const std::string& traceId,
                                const std::string& ip,
                                const std::string& method,
                                const std::string& path, int status,
                                size_t bytes, double elapsedMs,
                                const std::string& ua);

}  // namespace streaming
