#pragma once

#include "webhook/WebhookManager.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace streaming {

/**
 * Vhost: 一个虚拟主机（多租户）的配置。
 *
 * 按 HTTP Host 头 / RTMP tcUrl 的域名匹配，实现一台服务器托管多个业务：
 * 不同域名可使用不同的鉴权、回调 URL、转码 profile、HLS/DVR 策略与配额。
 */
struct Vhost {
    std::string name;                     // 域名，如 "live.example.com"
    std::string defaultApp = "live";
    bool enabled = true;

    // 鉴权
    bool enableAuth = false;
    std::string jwtSecret;                 // 为空则回落到全局 secret
    std::vector<std::string> allowApps;    // 空表示全部允许

    // HTTP 回调
    WebhookManager::Config webhooks;

    // HLS / DVR
    bool enableHls = true;
    int hlsSegmentDuration = 6;
    int hlsWindowSize = 5;
    bool enableDvr = false;
    std::string dvrDir = "./dvr";

    // 转码
    bool enableTranscode = false;
    std::string transcodeProfiles = "480p,720p";

    // 配额
    int maxPublishers = 0;     // 0 = 不限
    int maxViewers = 0;        // 0 = 不限
    int64_t maxBandwidthBps = 0;  // 0 = 不限

    // 运行期计数（原子，便于配额判断）
    std::atomic<int> publishers{0};
    std::atomic<int> viewers{0};
    std::atomic<int64_t> bandwidthBps{0};
};

using VhostPtr = std::shared_ptr<Vhost>;

/**
 * VhostManager: Vhost 注册表，支持运行时增删（配置热加载）。
 *
 * 匹配规则：精确域名 → 前缀通配 "*.example.com" → 默认 Vhost。
 * 线程安全。
 */
class VhostManager {
public:
    VhostManager();

    /// 用完整配置（名称→Vhost）替换当前表，并保证 "__default__" 存在。
    void reload(std::unordered_map<std::string, VhostPtr> vhosts);

    /// 单个增删（运行期 API 使用）
    void add(const VhostPtr& vh);
    void remove(const std::string& name);

    /// 按 Host 名查找 Vhost（不存在返回默认 Vhost）
    VhostPtr resolve(const std::string& host) const;

    /// 默认 Vhost
    VhostPtr defaultVhost() const;

    std::vector<std::string> listNames() const;

    /// 配额检查；true 表示放行
    bool canPublish(const VhostPtr& vh) const;
    bool canPlay(const VhostPtr& vh) const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, VhostPtr> vhosts_;
};

}  // namespace streaming
