#pragma once

#include "config/Vhost.hpp"
#include "core/NonCopyable.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace streaming {

/**
 * Config: 全局配置。
 *
 * 加载顺序：默认值 → 配置文件（KEY=VALUE）→ 环境变量（后者覆盖前者）。
 * 单例 Config::instance()。
 *
 * 配置文件为简单的 INI 风格：
 *   - 顶层（无 section）为全局标量键，如 HTTP_PORT、RTMP_PORT。
 *   - [vhost:<name>] 段定义一个虚拟主机，支持每 vhost 的鉴权、webhook
 *     回调 URL、HLS/DVR、转码与配额；[vhost:__default__] 配置默认 Vhost。
 *     段内键名见 applyVhostKv()（enable_auth / webhook_on_publish 等）。
 *
 * 环境变量前缀 STREAM_，例如 STREAM_HTTP_PORT、STREAM_RTMP_PORT。
 */
class Config : NonCopyable {
public:
    static Config& instance();

    /// 从可选配置文件加载，再叠加环境变量。返回 false 表示文件无法打开（非致命）。
    bool load(const std::string& file = "");

    uint16_t httpPort() const { return httpPort_; }
    uint16_t rtmpPort() const { return rtmpPort_; }
    int ioThreads() const { return ioThreads_; }
    int idleTimeoutSec() const { return idleTimeoutSec_; }

    int hlsSegmentDuration() const { return hlsSegmentDuration_; }
    int hlsWindowSize() const { return hlsWindowSize_; }
    std::string hlsOutputDir() const { return hlsOutputDir_; }
    int64_t cacheSizeMB() const { return cacheSizeMB_; }

    std::string jwtSecret() const { return jwtSecret_; }
    bool enableRateLimit() const { return enableRateLimit_; }
    int rateLimitPerSec() const { return rateLimitPerSec_; }
    int rateLimitBurst() const { return rateLimitBurst_; }
    std::string logLevel() const { return logLevel_; }
    bool logJson() const { return logJson_; }

    // 分路由限流策略（每分钟配额；0 = 关闭该策略）
    int ratePlaybackPerMin() const { return ratePlaybackPerMin_; }
    int rateApiPerMin() const { return rateApiPerMin_; }
    int rateAuthPerMin() const { return rateAuthPerMin_; }
    int rateTranscodePerMin() const { return rateTranscodePerMin_; }
    int rateAdminPerMin() const { return rateAdminPerMin_; }

    // 集群
    std::string nodeId() const { return nodeId_; }
    std::string originUrl() const { return originUrl_; }
    std::vector<std::string> clusterNodes() const { return clusterNodes_; }
    bool isEdge() const { return !originUrl_.empty(); }

    // 转码
    std::string transcodeProfiles() const { return transcodeProfiles_; }
    int transcodeConcurrency() const { return transcodeConcurrency_; }
    /// 终态转码任务保留时长（秒），超时由定时清理
    int transcodeTaskTtlSec() const { return transcodeTaskTtlSec_; }

    // SRT 接入端口（UDP）
    uint16_t srtPort() const { return srtPort_; }

    // Redis（HOST 为空 = 禁用，纯单机内存模式）
    std::string redisHost() const { return redisHost_; }
    uint16_t redisPort() const { return redisPort_; }
    int redisTimeoutMs() const { return redisTimeoutMs_; }
    std::string redisPassword() const { return redisPassword_; }

    /// 配置文件中声明的所有 Vhost（含 __default__，若存在）。
    /// 返回的是配置快照副本，线程安全。main.cpp 用它 reload 进 VhostManager。
    std::unordered_map<std::string, VhostPtr> vhosts() const;

    /// 重新加载配置文件（SIGHUP 热加载用）
    void loadFromFile(const std::string& path);

private:
    Config() = default;
    void loadFromEnv();
    void applyKv(const std::string& key, const std::string& value);
    void applyVhostKv(const VhostPtr& vh, const std::string& key,
                      const std::string& value);

    uint16_t httpPort_ = 8080;
    uint16_t rtmpPort_ = 1935;
    int ioThreads_ = 4;
    int idleTimeoutSec_ = 0;

    int hlsSegmentDuration_ = 6;
    int hlsWindowSize_ = 5;
    std::string hlsOutputDir_ = "./hls";
    int64_t cacheSizeMB_ = 256;

    std::string jwtSecret_ = "streaming-server-secret-key-2024";
    bool enableRateLimit_ = true;
    int rateLimitPerSec_ = 100;
    int rateLimitBurst_ = 200;
    std::string logLevel_ = "INFO";
    bool logJson_ = false;

    int ratePlaybackPerMin_ = 300;
    int rateApiPerMin_ = 100;
    int rateAuthPerMin_ = 10;
    int rateTranscodePerMin_ = 5;
    int rateAdminPerMin_ = 20;

    std::string nodeId_ = "node-1";
    std::string originUrl_;
    std::vector<std::string> clusterNodes_;

    std::string transcodeProfiles_ = "480p,720p";
    int transcodeConcurrency_ = 0;
    int transcodeTaskTtlSec_ = 3600;

    uint16_t srtPort_ = 19535;

    std::string redisHost_;
    uint16_t redisPort_ = 6379;
    int redisTimeoutMs_ = 50;
    std::string redisPassword_;

    // 配置文件中解析出的 vhost（name -> Vhost），仅在 loadFromFile 中写入
    std::unordered_map<std::string, VhostPtr> vhosts_;

    mutable std::mutex mu_;
};

}  // namespace streaming
