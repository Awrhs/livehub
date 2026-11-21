#pragma once

#include "core/NonCopyable.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

class HttpServer;
class HttpRequest;
class HttpResponse;
class HlsManager;
class StreamCache;
class MediaStream;
class TcpConnection;

/**
 * HlsHttpHandler: 把 HLS 源流挂载到 HTTP 路由，并统一处理 /live 下的单段资源，
 * 避免 m3u8、.flv、master.m3u8 因同形路由 /live/:x 产生注册顺序冲突。
 *
 * 路由（全部在 /live 下，按后缀分发）：
 *   GET /live/:stream.m3u8                  -> m3u8 播放列表
 *   GET /live/:stream.flv                   -> HTTP-FLV 长连接（委托给 flvResolver）
 *   GET /live/:stream/:seq.ts               -> TS 分片（sendfile 零拷贝 / 内存缓存）
 *
 * 简历亮点：sendfile 零拷贝、HTTP Range(206)、热点分片内存缓存、路由冲突治理
 */
class HlsHttpHandler : NonCopyable {
public:
    /// 给定 stream key 返回一个 MediaStream（用于 HTTP-FLV）
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string& key)>;

    HlsHttpHandler(HlsManager* manager, StreamCache* cache);

    /// 设置 HTTP-FLV 流解析器（设置后 .flv 请求才会被处理）
    void setFlvResolver(StreamResolver r) { flvResolver_ = std::move(r); }

    /// 播放鉴权：key 为流名；hlsPoll=true 表示 m3u8/分片请求（高频，不应触发 webhook）。
    /// 返回 false 时 *reason 为拒绝原因。未设置则不鉴权。
    using PlayAuth = std::function<bool(const HttpRequest& req, const std::string& key,
                                        bool hlsPoll, std::string* reason)>;
    void setPlayAuth(PlayAuth a) { playAuth_ = std::move(a); }

    void registerRoutes(HttpServer& server);

private:
    bool servePlaylist(const std::string& stream, class HttpResponse& resp,
                       const std::string& token = "");
    void serveFlv(const std::string& key, class HttpResponse& resp);

    HlsManager* manager_;
    StreamCache* cache_;
    StreamResolver flvResolver_;
    PlayAuth playAuth_;
    bool checkAuth(const HttpRequest& req, const std::string& key, bool hlsPoll,
                   HttpResponse& resp);
};

}  // namespace streaming
