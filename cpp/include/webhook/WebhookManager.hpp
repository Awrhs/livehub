#pragma once

#include "core/NonCopyable.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <memory>

namespace streaming {

/**
 * WebhookManager: 向业务后台异步（或同步）发送 HTTP POST JSON 事件回调。
 *
 * 对应 SRS 的 http hooks：on_connect / on_close / on_publish / on_unpublish
 * / on_play / on_stop / on_dvr / on_hls。
 *
 * 设计：
 * - 独立工作线程 + 阻塞式 socket（带连接/读超时），不占用 Reactor 线程；
 * - 通知类事件（on_close/dvr/hls）完全异步，失败按指数退避重试；
 * - 鉴权类事件（on_publish/on_play/on_connect）可同步等待结果，业务后台
 *   返回 JSON 含 "code":0 表示放行（兼容 SRS 的 "code":0 约定）。
 *
 * 线程安全：任意线程可调用 notify/authorize。
 *
 * 简历点：异步 HTTP 客户端、出站回调、鉴权链、退避重试。
 */
class WebhookManager : NonCopyable {
public:
    /// 鉴权决策结果
    struct AuthResult {
        bool allowed = true;
        std::string message;       // 业务返回的提示
        std::string redirectUrl;   // 可选 302 回源/跳转
    };

    using EventMap = std::map<std::string, std::string>;  // JSON kv

    struct Config {
        std::string onConnect;
        std::string onClose;
        std::string onPublish;
        std::string onUnpublish;
        std::string onPlay;
        std::string onStop;
        std::string onDvr;
        std::string onHls;
        int timeoutMs = 1500;       // 单次请求超时
        int maxRetries = 3;         // 异步事件重试次数
        bool enabled = true;
    };

    explicit WebhookManager(Config cfg);
    ~WebhookManager();

    void start();
    void stop();

    /// 设置/更新某事件的回调 URL（可运行期热更新）
    void setUrl(const std::string& event, const std::string& url);

    /// 原子替换整个回调配置（用于 vhost 切换 / SIGHUP 热加载）
    void setConfig(const Config& cfg);

    /// 异步通知（不等待结果，失败重试）
    void notify(const std::string& event, const EventMap& fields);

    /// 异步通知，使用指定 vhost 的回调配置（多租户）
    void notify(const std::string& event, const EventMap& fields,
                const Config& vhostCfg);

    /// 同步鉴权：阻塞当前线程直到业务返回或超时；超时默认放行(fail-open)
    AuthResult authorize(const std::string& event, const EventMap& fields);

    /// 同步鉴权，使用指定 vhost 的回调配置（多租户）
    AuthResult authorize(const std::string& event, const EventMap& fields,
                         const Config& vhostCfg);

    /// 异步鉴权：立即返回；结果在 webhook 工作线程中回调（未配置 URL 时在当前线程立即回调）。
    /// 网络错误/超时/响应无法解析均放行（fail-open）。
    using AuthDone = std::function<void(const AuthResult&)>;
    void authorizeAsync(const std::string& event, const EventMap& fields,
                        const Config& vhostCfg, AuthDone done);
    /// 该 vhost 配置下某事件是否配置了回调 URL
    bool hasHook(const std::string& event, const Config& vhostCfg) const;

    /// 全局启用/禁用
    void setEnabled(bool on) { enabled_.store(on); }
    bool enabled() const { return enabled_.load(); }

    uint64_t totalSent() const { return sent_.load(); }
    uint64_t totalFailed() const { return failed_.load(); }

private:
    // 同步等待状态：以 shared_ptr 与 Job 共享。
    // （原实现指向调用方栈上的 mutex/cv，等待超时返回后 worker 再访问即为 use-after-free）
    struct SyncWait {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        AuthResult result;
    };
    struct Job {
        std::string url;
        std::string body;
        bool needResult = false;
        int retries = 0;
        std::shared_ptr<SyncWait> sync;                     // 同步鉴权
        std::function<void(const AuthResult&)> onDone;      // 异步鉴权（在 worker 线程回调）
    };

    void workerLoop();
    bool postJson(const std::string& url, const std::string& body,
                  std::string& responseBody);
    bool parseAllow(const std::string& responseBody, AuthResult& out);
    std::string urlFor(const std::string& event) const;
    static std::string urlForEvent(const Config& cfg,
                                   const std::string& event);
    std::string buildBody(const EventMap& fields) const;

    // 内部实现：url 显式传入，支持多 vhost
    void notifyUrl(const std::string& url, int maxRetries,
                   const EventMap& fields);
    AuthResult authorizeUrl(const std::string& url, int timeoutMs,
                            const EventMap& fields);

    Config cfg_;
    mutable std::mutex cfgMu_;

    std::vector<std::thread> workers_;  // 多个工作线程，避免单个慢回调阻塞全部通知/鉴权
    static constexpr int kWorkers = 4;
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{true};

    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> failed_{0};
};

}  // namespace streaming
