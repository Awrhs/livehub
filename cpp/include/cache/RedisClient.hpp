#pragma once

#include "core/NonCopyable.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace streaming {

/**
 * RedisReply: 一条 RESP2 回复。
 */
struct RedisReply {
    enum Type { kNil, kSimple, kError, kInt, kBulk, kArray };
    Type type = kNil;
    std::string str;               // kSimple/kError/kBulk 的载荷
    int64_t integer = 0;           // kInt
    std::vector<RedisReply> arr;   // kArray

    bool isError() const { return type == kError; }
    bool isNil() const { return type == kNil; }
};

/**
 * RedisClient: 极简同步 RESP2 客户端（专用后台线程）。
 *
 * 用途（本工程内仅三个）：分布式限流（固定窗口 INCR+EXPIRE）、
 * JWT 黑名单跨节点共享、TS 分片缓存 L2 回填。
 *
 * 设计要点：
 * - 同步语义：限流/黑名单判定在请求主路径上需要同步答案，而 Reactor
 *   线程不可阻塞——因此客户端跑在专用后台线程上，调用方投递命令并
 *   限时等待 future（超时即降级，绝不长时间卡住请求）。
 * - 单连接复用 + 自动重连；socket 设 SO_RCVTIMEO/SO_SNDTIMEO，
 *   单次系统调用超时即失败。连接/IO 均由 worker 线程独占。
 * - 熔断：worker 内连续失败 ≥5 次 → OPEN 冷却 10s，期间所有命令立即
 *   失败；冷却后放一条试探（HALF_OPEN 语义），失败再开。熔断状态仅
 *   worker 单线程读写，无锁。
 * - fail-open：所有接口失败返回 false，由调用方退回单机内存实现，
 *   不阻断请求主路径。
 * - 未配置（host 为空）时 enabled()==false，所有调用直接返回 false。
 * - 协议编解码 encodeCommand/parseReply 为纯静态函数，可离线单测。
 *
 * configure() 可重复调用（SIGHUP 热改）：worker 每次重连时读取最新
 * 端点配置；端点为空则后续命令全部失败（等同禁用）。
 */
class RedisClient : NonCopyable {
public:
    static RedisClient& instance();

    /**
     * 配置并惰性启动 worker 线程。
     * host 为空 = 不启用（纯单机模式）。password 非空时连接后先 AUTH。
     */
    void configure(const std::string& host, uint16_t port, int timeoutMs,
                   const std::string& password = "");

    /// 是否已配置并运行（熔断打开时仍为 true，命令会快速失败）
    bool enabled() const;

    // ---- 高级接口（失败/超时/未配置一律返回 false，调用方自行降级）----

    /// 固定窗口限流：key 在 windowSec 窗口内自增，*count 返回当前计数。
    /// 窗口第一次计数时设置 EXPIRE。
    bool incrWithTtl(const std::string& key, int windowSec,
                     int64_t* count);

    /// 写 KV（带 TTL 秒）
    bool setWithTtl(const std::string& key, const std::string& val,
                    int ttlSec);

    /// 异步写 KV（fire-and-forget，不等待，绝不阻塞调用线程；缓存回填用）
    void setWithTtlAsync(const std::string& key, const std::string& val,
                         int ttlSec);

    /// 读 KV；key 不存在（nil）或失败均返回 false
    bool get(const std::string& key, std::string* val);

    bool del(const std::string& key);

    // ---- 协议层（纯函数，可单测）----

    /// 编码一条 RESP2 数组命令
    static std::string encodeCommand(const std::vector<std::string>& args);

    /**
     * 解析一条回复。
     * @return 消费的字节数（>0 成功）；0 = 数据不完整；-1 = 协议错误。
     */
    static int parseReply(const char* data, size_t len, RedisReply* out);

private:
    RedisClient() = default;
    ~RedisClient();

    struct Request {
        std::string cmd;  // 已编码命令
        std::shared_ptr<std::promise<RedisReply>> promise;  // 空 = 异步
    };

    // 调用线程侧
    bool command(const std::vector<std::string>& args, RedisReply* out);

    // worker 线程侧（fd_ 仅 worker 持有）
    void workerLoop();
    bool connect();
    void closeConn();
    bool sendAll(const std::string& data);
    bool sendAndRecv(const std::string& cmd, RedisReply* out);
    void recordFailure();
    void recordSuccess();
    bool breakerOpen() const;

    // 配置（configure 可热改）
    mutable std::mutex cfgMu_;
    std::string host_;
    uint16_t port_ = 6379;
    int timeoutMs_ = 50;
    std::string password_;

    // 命令队列
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<Request> q_;

    // worker
    std::atomic<bool> running_{false};
    std::thread worker_;
    int fd_ = -1;                       // 仅 worker 线程访问

    // 熔断（仅 worker 写）
    static constexpr int kBreakerThreshold = 5;
    static constexpr int64_t kBreakerCooldownMs = 10'000;
    std::atomic<int> consecutiveFailures_{0};
    std::atomic<int64_t> breakerOpenUntilMs_{0};
};

}  // namespace streaming
