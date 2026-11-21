/**
 * streaming_server 主入口（全功能装配）。
 *
 * 在一个 baseLoop + subReactor 线程池上同时运行：
 *   - HTTP  API/静态/metrics     （:8080）
 *   - RTMP  推流                  （:1935）
 *   - HLS   /live/<stream>.m3u8, /live/<stream>/<n>.ts
 *   - HTTP-FLV /live/<stream>.flv
 *   - 集群管理 API / 健康检查 / 回源
 *   - 转码管理
 *
 * 中间件：限流 → JWT 鉴权 → 路由 → 访问日志/metrics。
 * 优雅关闭：signalfd 捕获 SIGINT/SIGTERM。
 */
#include "api/HttpServer.hpp"
#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/Jwt.hpp"
#include "api/Json.hpp"
#include "api/StreamStats.hpp"
#include "api/StreamsApi.hpp"

#include "auth/TokenBlacklist.hpp"
#include "cache/RedisClient.hpp"
#include "cache/StreamCache.hpp"
#include "cluster/ClusterApi.hpp"
#include "cluster/HealthChecker.hpp"
#include "cluster/LoadBalancer.hpp"
#include "cluster/NodeRegistry.hpp"
#include "cluster/OriginEdge.hpp"
#include "cluster/ProxyHandler.hpp"
#include "cluster/StreamForwarder.hpp"
#include "config/Config.hpp"
#include "config/Vhost.hpp"
#include "core/AsyncFileWriter.hpp"
#include "core/Logger.hpp"
#include "core/SignalGuard.hpp"
#include "dvr/DiskWatermark.hpp"
#include "dvr/DvrHandler.hpp"
#include "dvr/DvrManager.hpp"
#include "hls/AbrManager.hpp"
#include "hls/HlsHttpHandler.hpp"
#include "hls/HlsManager.hpp"
#include "metrics/Metrics.hpp"
#include "metrics/SystemStats.hpp"
#include "middleware/AccessLog.hpp"
#include "middleware/AuthFilter.hpp"
#include "middleware/RateLimiter.hpp"
#include "middleware/RateLimitMiddleware.hpp"
#include "net/EventLoop.hpp"
#include "protocol/HttpFlvHandler.hpp"
#include "protocol/WsFlvHandler.hpp"
#include "rtmp/AvcParser.hpp"
#include "rtmp/RtmpConnection.hpp"
#include "rtmp/RtmpServer.hpp"
#include "rtmp/RtmpToHls.hpp"
#include "rtsp/RtspServer.hpp"
#include "srt/SrtServer.hpp"
#include "transcode/ProcessManager.hpp"
#include "transcode/TranscodeManager.hpp"
#include "webhook/WebhookManager.hpp"
#include "webrtc/WhepHandler.hpp"
#include "webrtc/WhipHandler.hpp"
#include "webrtc/WebRtcManager.hpp"

#include <execinfo.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>

using namespace streaming;

namespace {

/// 解析 "480p,720p" 形式的档位串（按预置名匹配）；无命中回退 480p。
/// /api/abr、/api/transcode、/api/profiles 共用，保证三处口径一致。
std::vector<Profile> parseProfileSpec(const std::string& spec) {
    std::vector<Profile> out;
    for (const char* name : {"360p", "480p", "720p", "1080p"}) {
        if (spec.find(name) != std::string::npos) {
            if (std::string(name) == "360p") out.push_back(Profile::p360());
            else if (std::string(name) == "480p") out.push_back(Profile::p480());
            else if (std::string(name) == "720p") out.push_back(Profile::p720());
            else out.push_back(Profile::p1080());
        }
    }
    if (out.empty()) out.push_back(Profile::p480());
    return out;
}

}  // namespace

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
namespace {
// 致命信号时打印调用栈（生产环境崩溃排查；需链接 -rdynamic 以获得符号名）
void crashHandler(int sig) {
    char head[64];
    int n = std::snprintf(head, sizeof(head), "\n*** FATAL signal %d, backtrace:\n", sig);
    if (n > 0) (void)!::write(STDERR_FILENO, head, static_cast<size_t>(n));
    void* frames[64];
    int depth = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, depth, STDERR_FILENO);
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
void installCrashHandler() {
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) ::signal(s, crashHandler);
}
}  // namespace
#endif

int main(int argc, char* argv[]) {
    ::signal(SIGPIPE, SIG_IGN);
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    installCrashHandler();  // sanitizer 构建由 sanitizer 自己报告
#endif
    // 在创建任何线程之前屏蔽终止/重载信号，由 SignalGuard 的 signalfd 统一处理
    SignalGuard::blockProcessSignals();

    // ---- 配置 ----
    std::string configFile = (argc > 1) ? argv[1] : "";
    Config::instance().load(configFile);
    const auto& cfg = Config::instance();

    // 日志：级别与 JSON 行格式走配置（STREAM_LOG_LEVEL / STREAM_LOG_JSON）
    {
        std::string lv = cfg.logLevel();
        if (lv == "TRACE") Logger::instance().setLevel(LogLevel::TRACE);
        else if (lv == "DEBUG") Logger::instance().setLevel(LogLevel::DEBUG);
        else if (lv == "WARN" || lv == "WARNING")
            Logger::instance().setLevel(LogLevel::WARN);
        else if (lv == "ERROR") Logger::instance().setLevel(LogLevel::ERROR);
        else Logger::instance().setLevel(LogLevel::INFO);
    }
    Logger::instance().setJsonMode(cfg.logJson());

    LOG_INFO("=== streaming_server starting ===");

    // JWT 密钥：不再内置默认值（公开源码中的默认值等于任何人可伪造 token）
    {
        static const char* kWeak[] = {"change-me-please", "change-me",
                                      "streaming-server-secret-key-2024",
                                      "secret"};
        std::string sec = Config::instance().jwtSecret();
        if (sec.empty()) {
            std::random_device rd;
            char buf[65];
            for (int i = 0; i < 32; ++i)
                std::snprintf(buf + i * 2, 3, "%02x", rd() & 0xFF);
            Config::instance().setJwtSecret(buf);
            LOG_WARN("JWT_SECRET 未配置：已生成随机密钥（重启后已签发 token 全部失效），"
                     "生产环境请显式配置");
        } else {
            bool weak = sec.size() < 16;
            for (const char* w : kWeak) weak = weak || sec == w;
            if (weak)
                LOG_WARN("JWT_SECRET 过弱或为公开默认值，任何人都可能伪造 token，请立即更换");
        }
        if (Config::instance().adminToken().empty())
            LOG_WARN("ADMIN_TOKEN 未配置：管理接口（/api/*）已关闭，仅健康检查与播放可用");
    }
    LOG_INFO("http=%u rtmp=%u ioThreads=%d hlsDir=%s edge=%d",
             cfg.httpPort(), cfg.rtmpPort(), cfg.ioThreads(),
             cfg.hlsOutputDir().c_str(), cfg.isEdge());

    // Redis 分布式状态（可选）：限流配额 / JWT 黑名单跨节点共享 / 缓存 L2。
    // HOST 未配置时纯单机内存模式，所有使用方自动降级（fail-open）。
    if (!cfg.redisHost().empty()) {
        RedisClient::instance().configure(cfg.redisHost(), cfg.redisPort(),
                                          cfg.redisTimeoutMs(),
                                          cfg.redisPassword());
        LOG_INFO("redis enabled: %s:%u timeout=%dms",
                 cfg.redisHost().c_str(), cfg.redisPort(),
                 cfg.redisTimeoutMs());
    }

    // ---- 核心循环（多 Reactor）----
    EventLoop baseLoop;

    auto rateLimiter = std::make_shared<RateLimiter>(
        cfg.rateLimitPerSec(), cfg.rateLimitBurst());
    // 分路由限流策略（最长前缀匹配；同名前缀共享配额桶，
    // 配额 0 = 关闭该策略）。默认值对齐旧版 Python 的 5 条策略。
    rateLimiter->addPolicy("playback", "/live/", cfg.ratePlaybackPerMin());
    rateLimiter->addPolicy("auth", "/api/token", cfg.rateAuthPerMin());
    rateLimiter->addPolicy("auth", "/api/logout", cfg.rateAuthPerMin());
    rateLimiter->addPolicy("transcode", "/api/transcode",
                           cfg.rateTranscodePerMin());
    rateLimiter->addPolicy("transcode", "/api/record",
                           cfg.rateTranscodePerMin());
    rateLimiter->addPolicy("transcode", "/api/forward",
                           cfg.rateTranscodePerMin());
    rateLimiter->addPolicy("transcode", "/api/abr",
                           cfg.rateTranscodePerMin());
    rateLimiter->addPolicy("admin", "/api/nodes", cfg.rateAdminPerMin());
    rateLimiter->addPolicy("admin", "/api/cluster", cfg.rateAdminPerMin());
    rateLimiter->addPolicy("admin", "/api/vhosts", cfg.rateAdminPerMin());
    rateLimiter->addPolicy("admin", "/api/dvr", cfg.rateAdminPerMin());
    rateLimiter->addPolicy("admin", "/api/streams", cfg.rateAdminPerMin());
    rateLimiter->addPolicy("api", "/api/", cfg.rateApiPerMin());
    auto jwt = std::make_shared<Jwt>(cfg.jwtSecret());
    auto tokenBlacklist = std::make_shared<TokenBlacklist>();

    // ---- 异步磁盘 IO（HLS/DVR/日志共用，写盘不阻塞 Reactor）----
    auto asyncWriter = std::make_shared<AsyncFileWriter>();
    asyncWriter->start();

    // ---- HLS / 缓存 ----
    auto hlsManager = std::make_shared<HlsManager>(
        cfg.hlsOutputDir(), cfg.hlsSegmentDuration(), cfg.hlsWindowSize());
    hlsManager->setAsyncWriter(asyncWriter.get());
    auto streamCache = std::make_shared<StreamCache>(
        static_cast<size_t>(cfg.cacheSizeMB()) * 1024 * 1024);

    // ---- Vhost 多租户（从配置加载，支持 SIGHUP 热加载）----
    auto vhostManager = std::make_shared<VhostManager>();
    // 用配置文件中的 [vhost:*] 段初始化（reload 保证 __default__ 存在）
    vhostManager->reload(cfg.vhosts());
    auto defaultVhost = vhostManager->defaultVhost();

    // ---- HTTP Webhook 回调（on_publish/play/unpublish/stop/dvr/hls）----
    auto webhooks = std::make_shared<WebhookManager>(defaultVhost->webhooks);
    webhooks->start();

    // ---- DVR / 录制 / 时移（提前创建，供推流回调使用）----
    DvrManager::Config dvrCfg;
    dvrCfg.baseDir = "./dvr";
    dvrCfg.segmentDurationMs =
        static_cast<uint32_t>(cfg.hlsSegmentDuration() * 1000);
    dvrCfg.timeshiftWindow = static_cast<uint32_t>(cfg.hlsWindowSize() * 2);
    dvrCfg.faststart = true;
    auto dvrManager = std::make_shared<DvrManager>(dvrCfg);
    dvrManager->setLoop(&baseLoop);
    dvrManager->setAsyncWriter(asyncWriter.get());

    // 磁盘水位保护：DVR 目录超过 90% 自动清理最旧录像
    DiskWatermark diskWatermark(dvrCfg.baseDir, 90, 80);
    diskWatermark.addManagedDir(dvrCfg.baseDir + "/recordings", ".mp4");
    diskWatermark.addManagedDir(dvrCfg.baseDir + "/timeshift", ".m4s");

    // ---- RTMP ----
    auto rtmpServer =
        std::make_shared<RtmpServer>(&baseLoop, cfg.rtmpPort(), "live");
    rtmpServer->setThreadNum(cfg.ioThreads());
    rtmpServer->setIdleTimeout(cfg.idleTimeoutSec());
    rtmpServer->setConnectionLimits(cfg.maxConnections(), cfg.maxConnectionsPerIp());

    // 回源（edge 模式）：本地未命中时向源站 HTTP-FLV pull。
    // 声明在 HTTP 解析器之前，使各 play resolver 能捕获并在运行时回退。
    std::shared_ptr<OriginEdge> edge;
    // 统一的播放流解析：先查本地，edge 模式下回源拉流
    auto resolvePlayStream =
        [&](const std::string& key) -> std::shared_ptr<MediaStream> {
        if (auto s = rtmpServer->findStream(key)) return s;
        if (edge) return edge->getOrPull(key);
        return nullptr;
    };

    // 推流 -> HLS 桥接（每路流一个 RtmpToHls）
    std::mutex bridgesMu;
    std::map<std::string, std::shared_ptr<RtmpToHls>> bridges;
    rtmpServer->setPublishHandler(
        [&](const std::shared_ptr<MediaStream>& ms) {
            auto bridge = std::make_shared<RtmpToHls>(
                ms, hlsManager.get(), cfg.hlsOutputDir(),
                cfg.hlsSegmentDuration(), cfg.hlsWindowSize());
            bridge->start();
            std::lock_guard<std::mutex> lk(bridgesMu);
            bridges[ms->key()] = bridge;
            Metrics::instance().gauge("rtmp_streams")->inc();

            // 注：on_publish webhook 已在 setAuthHandler 中同步调用（鉴权），
            // 这里不再重复通知。

            // 统计：订阅媒体包，记录字节/分辨率
            auto stats = StatsRegistry::instance().getOrCreate(ms->key());
            std::weak_ptr<StreamStats> wstats = stats;
            auto subId = ms->subscribe(
                [wstats](const MediaPacket& pkt) {
                    auto st = wstats.lock();
                    if (!st) return;
                    if (pkt.type == MediaPacket::kVideo) {
                        st->onVideoPacket(pkt.data.size());
                        // 从 sequence header 解析分辨率
                        if (pkt.isSequenceHeader && pkt.data.size() > 5) {
                            std::vector<uint8_t> sps, pps;
                            if (AvcParser::extractSpsPps(
                                    pkt.data.data() + 5,
                                    pkt.data.size() - 5, sps, pps)) {
                                if (sps.size() > 4) {
                                    int w = 0, h = 0;
                                    if (AvcParser::parseSpsDimensions(
                                            sps.data() + 4, sps.size() - 4, w,
                                            h)) {
                                        st->setVideoInfo(w, h, 30);
                                    }
                                }
                            }
                        }
                    } else if (pkt.type == MediaPacket::kAudio) {
                        st->onAudioPacket(pkt.data.size());
                    }
                });
            (void)subId;

            // DVR 时移自动开启（保留最近分片供回看）
            dvrManager->enableTimeshift(ms);
        });

    // 注：on_play webhook 已在 setAuthHandler 中同步调用（鉴权），
    // PlayHandler 无需再重复通知。

    // 推流结束（deleteStream / 连接断开）：on_unpublish + on_stop，清理桥接
    rtmpServer->setUnpublishHandler(
        [&](const std::string& app, const std::string& key) {
            {
                std::lock_guard<std::mutex> lk(bridgesMu);
                bridges.erase(key);
            }
            hlsManager->endStream(key);  // m3u8 追加 ENDLIST
            Metrics::instance().gauge("rtmp_streams")->dec();
            StatsRegistry::instance().remove(key);
            if (webhooks) {
                // 流已从注册表移除，无法取其 vhost，回退当前默认 vhost
                auto vh = vhostManager->defaultVhost();
                WebhookManager::EventMap fields;
                fields["app"] = app;
                fields["stream"] = key;
                fields["vhost"] = vh ? vh->name : "__default__";
                const auto& wcfg = vh ? vh->webhooks : defaultVhost->webhooks;
                fields["action"] = "on_unpublish";
                webhooks->notify("on_unpublish", fields, wcfg);
                fields["action"] = "on_stop";
                webhooks->notify("on_stop", fields, wcfg);
            }
        });

    // vhost 解析器：供 RtmpServer 在连接关闭时回收配额
    rtmpServer->setVhostResolver(
        [&](const std::string& host) -> VhostPtr {
            return vhostManager->resolve(host);
        });

    // RTMP 推/拉鉴权：按 vhost 配置执行 webhook 同步鉴权与（可选）JWT 校验，
    // 并做 vhost 启用状态与配额检查。
    // 推/拉流统一鉴权（RTMP 与 HTTP-FLV/HLS/WS-FLV 共用）
    //  - rawKey 可带查询串（stream?token=xxx）
    //  - countQuota：是否计入 vhost 推流/观看配额（HTTP 拉流无断开通知，不计）
    //  - callHook：是否调用同步 webhook（HLS 轮询高频，不调用）
    auto authorizeStream =
        [&](const std::string& host, const std::string& app,
            const std::string& rawKey, bool publish, std::string* reason,
            bool countQuota, bool callHook) -> bool {
            std::string key, query;
            RtmpConnection::splitStreamKey(rawKey, &key, &query);
            auto vh = vhostManager->resolve(host);
            if (!vh || !vh->enabled) {
                if (reason) *reason = "vhost disabled";
                return false;
            }
            // 配额
            if (publish) {
                if (!vhostManager->canPublish(vh)) {
                    if (reason) *reason = "publisher quota exceeded";
                    return false;
                }
            } else {
                if (!vhostManager->canPlay(vh)) {
                    if (reason) *reason = "viewer quota exceeded";
                    return false;
                }
            }
            // 应用白名单
            if (!vh->allowApps.empty()) {
                bool ok = false;
                for (const auto& a : vh->allowApps) {
                    if (a == app) { ok = true; break; }
                }
                if (!ok) {
                    if (reason) *reason = "app not allowed";
                    return false;
                }
            }
            // JWT 鉴权（vhost 开启时；密钥为空则回退全局 secret）
            if (vh->enableAuth) {
                std::string token;
                {
                    size_t p = 0;
                    while (p <= query.size()) {
                        size_t amp = query.find('&', p);
                        std::string kv = query.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
                        if (kv.compare(0, 6, "token=") == 0) token = kv.substr(6);
                        if (amp == std::string::npos) break;
                        p = amp + 1;
                    }
                }
                if (token.empty()) {
                    if (reason) *reason = "missing token";
                    return false;
                }
                const std::string& secret =
                    vh->jwtSecret.empty() ? cfg.jwtSecret() : vh->jwtSecret;
                Jwt v(secret);
                auto claims = v.verify(token);
                if (claims.empty()) {
                    if (reason) *reason = "invalid token";
                    return false;
                }
                auto claim = [&](const char* k) {
                    auto it = claims.find(k);
                    return it == claims.end() ? std::string() : it->second;
                };
                if (claim("role") != "admin") {
                    // 流 token：须绑定本流（stream 为空或 * 表示任意流）且 scope 匹配
                    std::string st = claim("stream");
                    if (!st.empty() && st != "*" && st != key) {
                        if (reason) *reason = "token not valid for this stream";
                        return false;
                    }
                    std::string scope = claim("scope");
                    if (scope.empty()) scope = "play";
                    if (scope != "all" && scope != (publish ? "publish" : "play")) {
                        if (reason) *reason = "token scope mismatch";
                        return false;
                    }
                }
                std::string jti = claim("jti");
                if (!jti.empty() && tokenBlacklist->contains(jti)) {
                    if (reason) *reason = "token revoked";
                    return false;
                }
            }
            // Webhook 同步鉴权（配置了对应 URL 时）；超时 fail-open
            const std::string event = publish ? "on_publish" : "on_play";
            WebhookManager::EventMap fields;
            fields["action"] = event;
            fields["app"] = app;
            fields["stream"] = key;   // 不含查询串，避免 token 泄露给 webhook
            fields["vhost"] = vh->name;
            if (callHook) {
                auto auth = webhooks->authorize(event, fields, vh->webhooks);
                if (!auth.allowed) {
                    if (reason) *reason = auth.message.empty() ? "denied by hook"
                                                               : auth.message;
                    return false;
                }
            }
            // 配额计数（通过后）
            if (countQuota) {
                if (publish) vh->publishers.fetch_add(1);
                else vh->viewers.fetch_add(1);
            }
            return true;
        };
    // RTMP 异步鉴权：本地校验（vhost/配额/应用/JWT）同步完成；webhook 异步执行，
    // 不阻塞 IO 线程。配额由 RtmpServer 在推/拉流成功时计入。
    rtmpServer->setAuthHandler(
        [&](const std::string& host, const std::string& app,
            const std::string& rawKey, bool publish,
            RtmpConnection::AuthDone done) {
            std::string reason;
            if (!authorizeStream(host, app, rawKey, publish, &reason,
                                 /*countQuota=*/false, /*callHook=*/false)) {
                done(false, reason);
                return;
            }
            auto vh = vhostManager->resolve(host);
            if (!vh) {
                done(true, "");
                return;
            }
            std::string key, query;
            RtmpConnection::splitStreamKey(rawKey, &key, &query);
            const std::string event = publish ? "on_publish" : "on_play";
            WebhookManager::EventMap fields;
            fields["action"] = event;
            fields["app"] = app;
            fields["stream"] = key;
            fields["vhost"] = vh->name;
            webhooks->authorizeAsync(event, fields, vh->webhooks,
                [done](const WebhookManager::AuthResult& r) {
                    done(r.allowed, r.allowed ? std::string()
                                              : (r.message.empty() ? "denied by hook" : r.message));
                });
        });

    // ---- HTTP ----
    HttpServer httpServer(&baseLoop, cfg.httpPort());
    httpServer.setThreadNum(cfg.ioThreads());
    httpServer.setIdleTimeout(cfg.idleTimeoutSec());
    httpServer.setConnectionLimits(cfg.maxConnections(), cfg.maxConnectionsPerIp());

    // 中间件顺序：trace-id -> 限流 -> vhost -> 鉴权 -> handler
    // （trace-id 必须最先：让限流 429 等拦截响应也带得上追踪头与日志）
    httpServer.use(
        [](const std::shared_ptr<TcpConnection>&, HttpRequest& req,
           HttpResponse& resp) -> bool {
            // 透传客户端 X-Request-ID；缺失/超长则生成 16 位 hex
            std::string tid = req.header("X-Request-ID");
            if (tid.empty() || tid.size() > 64) {
                static std::atomic<uint32_t> traceCtr{0};
                char buf[17];
                std::snprintf(buf, sizeof(buf), "%08x%08x",
                              static_cast<uint32_t>(
                                  Timestamp::now()
                                      .microSecondsSinceEpoch() &
                                  0xffffffffu),
                              traceCtr.fetch_add(1));
                tid = buf;
            }
            logging::setTraceId(tid);
            req.setAttribute("trace_id", tid);
            resp.addHeader("X-Trace-Id", tid);
            return true;
        });

    RateLimitMiddleware rlMw(rateLimiter);
    AuthFilter authMw(jwt);
    authMw.setAdminToken(cfg.adminToken());
    // JWT 注销黑名单（本地 + Redis 双层）挂进鉴权链：logout 过的 token 拒绝
    authMw.setBlacklist(tokenBlacklist);
    authMw.allow("/api/health");
    authMw.allow("/api/profiles");
    authMw.allow("/live/");
    authMw.allow("/metrics");
    // 中间件对象不可拷贝，用 std::ref 包装后交给 std::function
    if (cfg.enableRateLimit()) httpServer.use(std::ref(rlMw));

    // Vhost 解析中间件：按 HTTP Host 头解析 vhost，注入 req 属性；
    // 禁用的 vhost 返回 403。
    httpServer.use(
        [&](const std::shared_ptr<TcpConnection>&, HttpRequest& req,
            HttpResponse& resp) -> bool {
            // 注意：头名在解析时已统一小写，必须用 header() 查询
            std::string host = req.header("Host");
            auto vh = vhostManager->resolve(host);
            if (!vh || !vh->enabled) {
                resp.setStatusCode(HttpStatusCode::k403Forbidden);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"vhost disabled\"}");
                return false;
            }
            req.setAttribute("vhost", vh->name);
            return true;
        });

    httpServer.use(std::ref(authMw));

    AccessLog accessLog;
    httpServer.setAfterRequest(std::ref(accessLog));

    // HLS / HTTP-FLV：统一在 /live/:resource 按后缀分发，
    // 避免 .m3u8 与 .flv 两个同形路由 /live/:x 互相遮蔽。
    HlsHttpHandler hlsHandler(hlsManager.get(), streamCache.get());
    hlsHandler.setFlvResolver(
        [&](const std::string& key) -> std::shared_ptr<MediaStream> {
            return resolvePlayStream(key);
        });
    // HTTP 播放鉴权（此前 /live/ 完全不鉴权，vhost enable_auth 对 HLS/FLV 无效）
    auto httpPlayAuth = [&](const HttpRequest& req, const std::string& key,
                            bool hlsPoll, std::string* reason) {
        std::string raw = req.query().empty() ? key : key + "?" + req.query();
        return authorizeStream(req.header("Host"), "live", raw, false, reason,
                               /*countQuota=*/false, /*callHook=*/!hlsPoll);
    };
    hlsHandler.setPlayAuth(httpPlayAuth);
    hlsHandler.registerRoutes(httpServer);

    // WebSocket-FLV 低延迟直播（HTTP Upgrade）
    WsFlvHandler wsFlvHandler(
        [&](const std::string& key) -> std::shared_ptr<MediaStream> {
            if (auto s = resolvePlayStream(key)) {
                if (auto st = StatsRegistry::instance().get(key))
                    st->addViewer();
                return s;
            }
            return nullptr;
        });
    wsFlvHandler.setPlayAuth(
        [&](const HttpRequest& req, const std::string& key, std::string* reason) {
            return httpPlayAuth(req, key, false, reason);
        });
    wsFlvHandler.attach(httpServer);

    // ---- WebRTC（WHIP 上行 / WHEP 下行）----
    // 须在 WhipHandler/WhepHandler 之前构造（它们的构造函数立即取 webrtc.get()）
    auto webrtc =
        std::make_shared<WebRtcManager>(&baseLoop, 50000, 1000);

    // WebRTC WHIP / WHEP
    auto streamResolver =
        [&](const std::string& key,
            bool createIfMissing) -> std::shared_ptr<MediaStream> {
        if (createIfMissing) return rtmpServer->getOrCreateStream(key);
        return rtmpServer->findStream(key);
    };
    WhipHandler whipHandler(webrtc.get(), streamResolver);
    whipHandler.registerRoutes(httpServer);
    WhepHandler whepHandler(webrtc.get(),
                            [&](const std::string& key) {
                                return resolvePlayStream(key);
                            });
    whepHandler.registerRoutes(httpServer);

    // DVR / 录制 / 时移
    DvrHandler dvrHandler(
        dvrManager.get(),
        [&](const std::string& key) -> std::shared_ptr<MediaStream> {
            return rtmpServer->findStream(key);
        });
    dvrHandler.attach(httpServer);

    // ---- 转码 + ABR 多码率（须在 StreamsApi / ABR 路由之前构造，
    //      因为这些 lambda 会捕获 abrManager / transcodeManager）----
    auto transcodeManager =
        std::make_shared<TranscodeManager>(&baseLoop,
                                            cfg.transcodeConcurrency());
    transcodeManager->start();
    auto abrManager = std::make_shared<AbrManager>(
        transcodeManager.get(),
        "rtmp://127.0.0.1:" + std::to_string(cfg.rtmpPort()),
        "http://127.0.0.1:" + std::to_string(cfg.httpPort()));

    // 统计 API + ABR 主播放列表
    StreamsApi streamsApi;
    streamsApi.setStreamLister([&]() -> std::vector<StreamInfo> {
        std::vector<StreamInfo> out;
        int64_t now = Timestamp::now().secondsSinceEpoch();
        for (const auto& key : rtmpServer->listKeys()) {
            auto ms = rtmpServer->findStream(key);
            auto st = StatsRegistry::instance().get(key);
            StreamInfo info;
            info.key = key;
            if (ms) {
                info.hasPublisher = ms->hasPublisher();
                info.viewers = ms->subscriberCount();
            }
            if (st) {
                info.videoBytes = st->videoBytes();
                info.audioBytes = st->audioBytes();
                info.width = st->width();
                info.height = st->height();
                info.fps = st->fps();
                info.audioSampleRate = st->audioSampleRate();
                info.audioChannels = st->audioChannels();
                info.videoBitrate = st->videoBitrate();
                info.audioBitrate = st->audioBitrate();
                info.uptimeSec = now - st->startedSec();
            }
            out.push_back(info);
        }
        return out;
    });
    streamsApi.setStreamKiller([&](const std::string& key) -> bool {
        auto ms = rtmpServer->findStream(key);
        if (!ms) return false;
        ms->setHasPublisher(false);
        rtmpServer->removeStream(key);
        StatsRegistry::instance().remove(key);
        abrManager->disable(key);
        dvrManager->stopRecord(key);
        return true;
    });
    streamsApi.setAbrMaster(
        [&](const std::string& key) { return abrManager->masterPlaylist(key); });
    streamsApi.attach(httpServer);

    // ABR 控制 API：POST /api/abr/:stream?profiles=480p,720p
    httpServer.router().post(
        "/api/abr/:stream",
        [&](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("stream");
            // 从 query string 中解析 profiles=xxx
            std::string prof;
            const std::string& qs = req.query();
            auto ppos = qs.find("profiles=");
            if (ppos != std::string::npos) {
                size_t s = ppos + 9;
                auto end = qs.find('&', s);
                prof = qs.substr(s, end == std::string::npos ? std::string::npos
                                                             : end - s);
            }
            if (prof.empty()) prof = cfg.transcodeProfiles();
            std::vector<Profile> profiles = parseProfileSpec(prof);
            std::string input =
                "rtmp://127.0.0.1:" + std::to_string(cfg.rtmpPort()) +
                "/live/" + key;
            abrManager->enable(key, input, profiles);
            JsonValue o = JsonValue::object();
            o.set("stream", JsonValue::str(key));
            o.set("abr", JsonValue::boolean(true));
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(o.dump());
        });

    // ---- 集群 ----
    auto nodeRegistry = std::make_shared<NodeRegistry>();
    auto loadBalancer = std::make_shared<LoadBalancer>(
        nodeRegistry.get(), LbStrategy::kConsistentHash);
    auto healthChecker = std::make_shared<HealthChecker>(
        &baseLoop, nodeRegistry.get(), 5, 3, 15, 2000);
    if (cfg.isEdge()) {
        // 解析 originUrl（http://host:port）
        std::string host = cfg.originUrl();
        uint16_t port = 80;
        auto schemePos = host.find("://");
        if (schemePos != std::string::npos) host = host.substr(schemePos + 3);
        auto colon = host.find(':');
        if (colon != std::string::npos) {
            port = static_cast<uint16_t>(std::stoi(host.substr(colon + 1)));
            host = host.substr(0, colon);
        }
        edge = std::make_shared<OriginEdge>(&baseLoop, host, port, "live");
    }
    // 从配置加载集群节点：CLUSTER_NODES="ip:httpPort:rtmpPort,ip:..."
    {
        int idx = 0;
        for (const auto& spec : cfg.clusterNodes()) {
            std::string s = spec;
            std::string ip = s;
            uint16_t hp = 8080, rp = 1935;
            auto c1 = s.find(':');
            if (c1 != std::string::npos) {
                ip = s.substr(0, c1);
                auto rest = s.substr(c1 + 1);
                auto c2 = rest.find(':');
                try {
                    if (c2 != std::string::npos) {
                        hp = static_cast<uint16_t>(std::stoi(rest.substr(0, c2)));
                        rp = static_cast<uint16_t>(std::stoi(rest.substr(c2 + 1)));
                    } else {
                        hp = static_cast<uint16_t>(std::stoi(rest));
                    }
                } catch (...) {
                    // 忽略格式错误的节点
                }
            }
            if (!ip.empty()) {
                nodeRegistry->add(std::make_shared<Node>(
                    "node-" + std::to_string(idx++), ip, hp, rp));
            }
        }
        if (idx > 0) healthChecker->start();
    }
    ProxyHandler proxy(&baseLoop);
    proxy.setUpstreamSelector(
        [lb = loadBalancer.get()](const std::string& key)
            -> std::pair<std::string, uint16_t> {
            auto n = lb->selectNode(key);
            if (!n) return {"", 0};
            return {n->ip, n->httpPort};
        });
    proxy.registerRoutes(httpServer);
    ClusterApi clusterApi(nodeRegistry.get(), loadBalancer.get(),
                          healthChecker.get());
    clusterApi.registerRoutes(httpServer);

    // ---- RTSP ----
    auto rtspServer = std::make_shared<RtspServer>(&baseLoop, 8554);
    rtspServer->setThreadNum(cfg.ioThreads());
    rtspServer->setStreamResolver(
        [&](const std::string& key) -> std::shared_ptr<MediaStream> {
            return resolvePlayStream(key);
        });

    // ---- 基础 API ----
    // （/api/health 在 SRT 之后注册：需要统计 RTMP + SRT 两类活跃连接）

    // 签发 token（仅管理员；由 AuthFilter 保证）：
    //   POST /api/token?stream=<名>&scope=publish|play|all&ttl=<秒>&role=stream|admin
    // 此前该接口无需鉴权，任何人都能领取有效 token。
    httpServer.router().post(
        "/api/token",
        [&](const HttpRequest& req, HttpResponse& resp) {
            std::string role = req.param("role").empty() ? "stream" : req.param("role");
            std::string scope = req.param("scope").empty() ? "play" : req.param("scope");
            if ((role != "stream" && role != "admin") ||
                (scope != "play" && scope != "publish" && scope != "all")) {
                resp.setStatusCode(HttpStatusCode::k400BadRequest);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"invalid role or scope\"}");
                return;
            }
            int64_t ttl = 3600;
            if (!req.param("ttl").empty()) {
                try { ttl = std::stoll(req.param("ttl")); } catch (...) { ttl = -1; }
            }
            if (ttl <= 0 || ttl > 7 * 24 * 3600) {
                resp.setStatusCode(HttpStatusCode::k400BadRequest);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"ttl must be 1..604800\"}");
                return;
            }
            std::map<std::string, std::string> payload;
            payload["sub"] = req.param("sub").empty() ? "client" : req.param("sub");
            payload["role"] = role;
            if (role == "stream") {
                payload["scope"] = scope;
                payload["stream"] = req.param("stream").empty() ? "*" : req.param("stream");
            }
            payload["jti"] = std::to_string(
                Timestamp::now().microSecondsSinceEpoch());
            std::string token = jwt->sign(payload, ttl);
            JsonValue obj = JsonValue::object();
            obj.set("token", JsonValue::str(token));
            obj.set("expires_in", JsonValue::number(static_cast<double>(ttl)));
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(obj.dump());
        });

    httpServer.router().post(
        "/api/logout",
        [&](const HttpRequest& req, HttpResponse& resp) {
            std::string jti = req.attribute("jti");
            if (!jti.empty()) {
                int64_t expire =
                    Timestamp::now().secondsSinceEpoch() + 3600;
                tokenBlacklist->add(jti, expire);
            }
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody("{\"status\":\"logged out\"}");
        });

    // ---- 转码任务管理：GET 列表+统计 / POST 提交（可带优先级）----
    httpServer.router().get(
        "/api/transcode",
        [&](const HttpRequest&, HttpResponse& resp) {
            JsonValue tasks = JsonValue::array();
            for (const auto& t : transcodeManager->list()) {
                JsonValue o = JsonValue::object();
                o.set("id", JsonValue::str(t.id));
                o.set("input", JsonValue::str(t.inputUrl));
                o.set("output", JsonValue::str(t.outputPrefix));
                o.set("state", JsonValue::str(taskStateName(t.state)));
                o.set("priority",
                      JsonValue::number(static_cast<double>(t.priority)));
                std::string profNames;
                for (const auto& p : t.profiles) {
                    if (!profNames.empty()) profNames += ",";
                    profNames += p.name;
                }
                o.set("profiles", JsonValue::str(profNames));
                o.set("submitted_at_ms",
                      JsonValue::number(
                          static_cast<double>(t.submittedAtMs)));
                o.set("started_at_ms",
                      JsonValue::number(static_cast<double>(t.startedAtMs)));
                o.set("ended_at_ms",
                      JsonValue::number(static_cast<double>(t.endedAtMs)));
                if (!t.error.empty()) o.set("error", JsonValue::str(t.error));
                tasks.append(o);
            }
            auto st = transcodeManager->stats();
            JsonValue statsO = JsonValue::object();
            statsO.set("pending",
                       JsonValue::number(static_cast<double>(st.pending)));
            statsO.set("running",
                       JsonValue::number(static_cast<double>(st.running)));
            statsO.set("completed",
                       JsonValue::number(static_cast<double>(st.completed)));
            statsO.set("failed",
                       JsonValue::number(static_cast<double>(st.failed)));
            statsO.set("cancelled",
                       JsonValue::number(static_cast<double>(st.cancelled)));
            statsO.set("avg_wait_ms", JsonValue::number(st.avgWaitMs));
            JsonValue root = JsonValue::object();
            root.set("tasks", tasks);
            root.set("stats", statsO);
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(root.dump());
        });

    httpServer.router().post(
        "/api/transcode",
        [&](const HttpRequest& req, HttpResponse& resp) {
            // 简化：body 形式 input=<url>&output=<prefix>&profiles=480p,720p
            //            可选 priority=<int>（大者先调度）
            std::map<std::string, std::string> form;
            size_t i = 0;
            while (i < req.body().size()) {
                auto amp = req.body().find('&', i);
                std::string pair = req.body().substr(
                    i, amp == std::string::npos ? std::string::npos
                                                 : amp - i);
                auto eq = pair.find('=');
                if (eq != std::string::npos)
                    form[pair.substr(0, eq)] = pair.substr(eq + 1);
                if (amp == std::string::npos) break;
                i = amp + 1;
            }
            int priority = 0;
            if (form.count("priority")) {
                try {
                    priority = std::stoi(form["priority"]);
                } catch (...) {
                    resp.setStatusCode(HttpStatusCode::k400BadRequest);
                    resp.setContentType("application/json");
                    resp.setBody("{\"error\":\"invalid priority\"}");
                    return;
                }
            }
            std::vector<Profile> profiles =
                parseProfileSpec(form.count("profiles") ? form["profiles"]
                                                        : "480p");
            std::string id = transcodeManager->submit(
                form["input"], form["output"], profiles, true, priority);
            JsonValue obj = JsonValue::object();
            obj.set("task_id", JsonValue::str(id));
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(obj.dump());
        });

    httpServer.router().get(
        "/metrics",
        [](const HttpRequest&, HttpResponse& resp) {
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("text/plain; version=0.0.4");
            resp.setBody(Metrics::instance().render());
        });

    // ---- SRT 接入（UDP 19535）：TS 推流 ingest / TS 拉流 ----
    auto srtServer = std::make_shared<SrtServer>(&baseLoop, cfg.srtPort());
    srtServer->setStreamResolver(
        [&](const std::string& key) -> std::shared_ptr<MediaStream> {
            // SRT 推流：注册到 RTMP 流表，复用 HLS 桥接 / 统计 / DVR 等整条分发链。
            // 流名已有推流者（RTMP 或 SRT）时拒绝，避免两路数据写入同一流。
            if (auto cur = rtmpServer->findStream(key)) {
                if (cur->hasPublisher()) return nullptr;
            }
            auto ms = rtmpServer->getOrCreateStream(key);
            ms->setHasPublisher(true);
            return ms;
        });
    srtServer->setAuthHandler(
        [&](const std::string& rawKey, bool publish, std::string* reason) {
            return authorizeStream("", "live", rawKey, publish, reason,
                                   /*countQuota=*/false, /*callHook=*/true);
        });
    srtServer->setPlayResolver(
        [&](const std::string& key) -> std::shared_ptr<MediaStream> {
            return resolvePlayStream(key);
        });
    // SRT 推流断开：从 RTMP 流表移除（removeStream 会触发 on_unpublish/on_stop
    // webhook 与桥接/统计清理），与 RTMP deleteStream 走同一条回收路径。
    srtServer->setUnpublishHandler(
        [&](const std::string& key, const std::shared_ptr<MediaStream>& ms) {
            rtmpServer->removeStream(key, ms);
        });
    srtServer->start();

    // ---- 健康检查（含系统指标；需在 SRT 之后注册以统计活跃连接）----
    httpServer.router().get(
        "/api/health",
        [&](const HttpRequest&, HttpResponse& resp) {
            auto& sys = SystemStats::instance();
            JsonValue obj = JsonValue::object();
            obj.set("status", JsonValue::str("ok"));
            obj.set("service", JsonValue::str("streaming-server"));
            obj.set("cpu_usage", JsonValue::number(sys.cpuUsage()));
            obj.set("memory_usage", JsonValue::number(sys.memUsage()));
            obj.set("loadavg1", JsonValue::number(sys.loadAvg1()));
            obj.set("uptime_sec",
                    JsonValue::number(
                        static_cast<double>(sys.uptimeSec())));
            obj.set("active_connections",
                    JsonValue::number(static_cast<double>(
                        rtmpServer->streamCount() +
                        srtServer->sessionCount())));
            obj.set("timestamp",
                    JsonValue::str(Timestamp::now().toFormattedString(true)));
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(obj.dump());
        });

    // ---- 转码档位列表（按 vhost）----
    httpServer.router().get(
        "/api/profiles",
        [&](const HttpRequest& req, HttpResponse& resp) {
            // vhost 中间件注入的租户名；未注入（异常路径）回退默认
            std::string vname = req.attribute("vhost", "__default__");
            auto vh = vhostManager->resolve(vname);
            std::string spec = (vh && !vh->transcodeProfiles.empty())
                                   ? vh->transcodeProfiles
                                   : cfg.transcodeProfiles();
            std::vector<Profile> profiles = parseProfileSpec(spec);
            JsonValue arr = JsonValue::array();
            for (const auto& p : profiles) {
                JsonValue o = JsonValue::object();
                o.set("name", JsonValue::str(p.name));
                o.set("width", JsonValue::number(static_cast<double>(p.width)));
                o.set("height",
                      JsonValue::number(static_cast<double>(p.height)));
                o.set("video_bitrate",
                      JsonValue::number(static_cast<double>(p.videoBitrate)));
                o.set("audio_bitrate",
                      JsonValue::number(static_cast<double>(p.audioBitrate)));
                o.set("fps", JsonValue::number(static_cast<double>(p.fps)));
                arr.append(o);
            }
            JsonValue o = JsonValue::object();
            o.set("vhost", JsonValue::str(vname));
            o.set("profiles", arr);
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(o.dump());
        });

    // ---- 多平台转推（RTMP forward）：POST /api/forward/:stream?url=rtmp://... ----
    std::mutex forwardersMu;
    std::map<std::string, std::shared_ptr<StreamForwarder>> forwarders;
    auto getForwarder =
        [&](const std::string& key) -> std::shared_ptr<StreamForwarder> {
        // 只能转推正在发布的流；不存在返回 nullptr，避免凭空创建流并误触发 onPublish
        auto ms = rtmpServer->findStream(key);
        if (!ms) return nullptr;
        std::lock_guard<std::mutex> lk(forwardersMu);
        auto it = forwarders.find(key);
        if (it != forwarders.end()) return it->second;
        auto fwd = std::make_shared<StreamForwarder>(&baseLoop, ms);
        fwd->start();
        forwarders[key] = fwd;
        return fwd;
    };
    httpServer.router().post(
        "/api/forward/:stream",
        [&](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("stream");
            std::string url;
            const std::string& qs = req.query();
            auto upos = qs.find("url=");
            if (upos != std::string::npos) {
                size_t s = upos + 4;
                auto end = qs.find('&', s);
                url = qs.substr(s, end == std::string::npos
                                        ? std::string::npos : end - s);
            }
            if (url.empty() && !req.body().empty()) url = req.body();
            if (url.empty()) {
                resp.setStatusCode(HttpStatusCode::k400BadRequest);
                resp.setBody("{\"error\":\"missing url\"}");
                return;
            }
            // scheme 白名单：转推只接受 rtmp/rtmps（StreamForwarder 内部
            // parseRtmpUrl 同样严格校验，此处提前给出明确 400）
            if (url.rfind("rtmp://", 0) != 0 &&
                url.rfind("rtmps://", 0) != 0) {
                resp.setStatusCode(HttpStatusCode::k400BadRequest);
                resp.setContentType("application/json");
                resp.setBody(
                    "{\"error\":\"unsupported url scheme, "
                    "expect rtmp:// or rtmps://\"}");
                return;
            }
            auto fwd = getForwarder(key);
            if (!fwd) {
                resp.setStatusCode(HttpStatusCode::k404NotFound);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"stream not found\"}");
                return;
            }
            uint64_t id = fwd->addTarget(url);
            JsonValue o = JsonValue::object();
            o.set("stream", JsonValue::str(key));
            o.set("target_id", JsonValue::number(static_cast<double>(id)));
            o.set("url", JsonValue::str(url));
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(o.dump());
        });

    // ---- Vhost 管理 API ----
    httpServer.router().get(
        "/api/vhosts",
        [&](const HttpRequest&, HttpResponse& resp) {
            JsonValue arr = JsonValue::array();
            for (const auto& name : vhostManager->listNames()) {
                arr.append(JsonValue::str(name));
            }
            JsonValue o = JsonValue::object();
            o.set("vhosts", arr);
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(o.dump());
        });

    // ---- 录制列表 API ----
    httpServer.router().get(
        "/api/dvr/recordings",
        [&](const HttpRequest&, HttpResponse& resp) {
            auto recs = dvrManager->listRecordings();
            JsonValue arr = JsonValue::array();
            for (const auto& r : recs) {
                JsonValue o = JsonValue::object();
                o.set("key", JsonValue::str(r.key));
                o.set("path", JsonValue::str(r.path));
                o.set("size",
                      JsonValue::number(static_cast<double>(r.sizeBytes)));
                o.set("mtime",
                      JsonValue::number(static_cast<double>(r.startEpoch)));
                arr.append(o);
            }
            JsonValue o = JsonValue::object();
            o.set("recordings", arr);
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(o.dump());
        });

    // ---- 定时任务：HLS 过期回收 / 限流桶清理 / 黑名单清理 / 磁盘水位 ----
    baseLoop.runEvery(10.0, [&] {
        int64_t nowMs = Timestamp::now().microSecondsSinceEpoch() / 1000;
        hlsManager->sweep(nowMs, 60);
        rateLimiter->sweep(300);
        tokenBlacklist->sweep(Timestamp::now().secondsSinceEpoch());
        Metrics::instance().gauge("rtmp_streams_active")
            ->set(static_cast<int64_t>(rtmpServer->streamCount()));
        Metrics::instance().gauge("srt_sessions")
            ->set(static_cast<int64_t>(srtServer->sessionCount()));
        // 系统指标（Prometheus，千分比整数：750 = 75%）
        Metrics::instance().gauge("system_cpu_permille", "CPU usage in per-mille")
            ->set(static_cast<int64_t>(
                SystemStats::instance().cpuUsage() * 1000.0));
        double mem = SystemStats::instance().memUsage();
        if (mem >= 0) {
            Metrics::instance()
                .gauge("system_mem_permille", "Memory usage in per-mille")
                ->set(static_cast<int64_t>(mem * 1000.0));
        }
        // 转码任务指标 + 终态任务 TTL 清理
        auto tstats = transcodeManager->stats();
        Metrics::instance()
            .gauge("transcode_tasks_pending", "Queued transcode tasks")
            ->set(static_cast<int64_t>(tstats.pending));
        Metrics::instance()
            .gauge("transcode_tasks_running", "Running transcode tasks")
            ->set(static_cast<int64_t>(tstats.running));
        transcodeManager->sweepFinished(cfg.transcodeTaskTtlSec());
    });

    // 每分钟检查磁盘水位，超阈值清理最旧录像
    auto watermarkTimer = baseLoop.runEvery(60.0, [&] {
        diskWatermark.check();
    });
    (void)watermarkTimer;

    // ---- 优雅关闭 + SIGHUP 配置热加载 ----
    SignalGuard signals(&baseLoop);
    signals.addSignal(SIGHUP);
    std::atomic<bool> reloading{false};
    signals.setSignalCallback([&](int signo) {
        if (signo == SIGHUP) {
            if (reloading.exchange(true)) return;
            LOG_INFO("SIGHUP received, reloading config/vhosts...");
            // 重新加载配置文件（不中断在途连接）
            if (!configFile.empty()) Config::instance().loadFromFile(configFile);
            // Redis 端点热更新（worker 下次重连读新配置）
            if (!Config::instance().redisHost().empty()) {
                RedisClient::instance().configure(
                    Config::instance().redisHost(),
                    Config::instance().redisPort(),
                    Config::instance().redisTimeoutMs(),
                    Config::instance().redisPassword());
            }
            // 重建整个 vhost 表（[vhost:*] 段）
            vhostManager->reload(Config::instance().vhosts());
            // 用默认 vhost 的回调配置刷新全局 WebhookManager（默认租户）
            auto def = vhostManager->defaultVhost();
            if (webhooks && def) webhooks->setConfig(def->webhooks);
            LOG_INFO("config reloaded");
            reloading.store(false);
            return;
        }
        LOG_INFO("shutting down...");
        // 其余资源在 baseLoop 退出后按序关闭（见 main 末尾）
        // SignalGuard 在终止类信号回调返回后会调用 baseLoop.quit()
    });

    // ---- 启动 ----
    healthChecker->start();
    rtmpServer->start();
    rtspServer->start();
    httpServer.start();

    LOG_INFO("streaming_server ready: http=:%u rtmp=:%u rtsp=:8554 "
             "srt=:19535 webrtc-udp=50000-50999",
             cfg.httpPort(), cfg.rtmpPort());
    baseLoop.loop();

    // ---- 有序关闭 ----
    // 必须先关闭所有连接并 join IO 线程，再析构其它对象：
    // 否则 IO 线程上的回调会访问已析构的局部对象（带活跃连接退出时必崩）。
    LOG_INFO("closing connections and stopping IO threads...");
    httpServer.stop();
    rtmpServer->stop();
    rtspServer->stop();
    srtServer->stop();
    for (const auto& key : hlsManager->keys()) hlsManager->endStream(key);
    transcodeManager->stop();
    healthChecker->stop();
    webhooks->stop();
    asyncWriter->stop();  // 等待已排队的分片写盘完成

    LOG_INFO("streaming_server stopped");
    Logger::instance().flush();
    // 所有组件已有序关闭；跳过全局/静态析构直接退出。
    // 部分第三方库（如 Ubuntu 24.04 的 libsrt-gnutls 1.5）在全局析构阶段会段错误。
    std::fflush(nullptr);
    ::_exit(0);
}
