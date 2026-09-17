#pragma once

#include "core/NonCopyable.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

class HttpServer;
class MediaStream;

/**
 * HttpFlvHandler: HTTP-FLV 直播。
 *
 * 路由 GET /live/:stream.flv
 *
 * 响应头发出后保持长连接，按 FLV tag 持续推送：
 *   1) FLV header
 *   2) script tag（onMetaData，来自 MediaStream）
 *   3) sequence header（video/audio）+ GOP 缓存
 *   4) 实时订阅包
 *
 * 客户端断开时自动取消订阅。
 */
class HttpFlvHandler : NonCopyable {
public:
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string&)>;

    explicit HttpFlvHandler(StreamResolver resolver);

    void registerRoutes(HttpServer& server);

    /// 直接处理一次 .flv 请求（供统一 /live 路由分发调用，避免路由冲突）。
    /// key 已去掉 .flv 后缀；调用方负责路径与安全校验。
    static void serve(const std::string& key, class HttpResponse& resp,
                      const StreamResolver& resolver);

private:
    StreamResolver resolver_;
};

}  // namespace streaming
