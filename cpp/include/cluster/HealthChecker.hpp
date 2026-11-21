#pragma once

#include "cluster/Node.hpp"
#include "cluster/NodeRegistry.hpp"
#include "core/NonCopyable.hpp"
#include "net/EventLoop.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <mutex>

namespace streaming {

struct Probe;  // 单次健康探测（定义于 .cpp），需访问 onProbeResult

/**
 * Circuit breaker state.
 *   CLOSED   : 正常转发，失败累计
 *   OPEN     : 熔断，直接失败，冷却后转 HALF_OPEN
 *   HALF_OPEN: 放一次探测，成功转 CLOSED，失败转 OPEN
 */
enum class CircuitState { kClosed, kOpen, kHalfOpen };

struct CircuitStatus {
    CircuitState state = CircuitState::kClosed;
    int failures = 0;
    int successes = 0;
    int64_t openedAtMs = 0;
};

/**
 * HealthChecker: 周期性 HTTP 健康探测 + 熔断器。
 *
 * - 主动探测：每 intervalSec 对每个节点 GET {httpBase}/api/health。
 * - 被动熔断：连续失败达 threshold 次 → OPEN；冷却 cooldownSec 后 HALF_OPEN
 *   试探一次；成功恢复 CLOSED。
 * - 状态变更更新 Node::healthy。
 *
 * 线程模型：在传入的 EventLoop 线程中运行。
 */
class HealthChecker : NonCopyable {
public:
    using StateChangeCallback =
        std::function<void(const NodePtr&, CircuitState /*old*/,
                           CircuitState /*now*/)>;

    HealthChecker(EventLoop* loop, NodeRegistry* registry,
                  int intervalSec = 5, int failureThreshold = 3,
                  int cooldownSec = 15, int timeoutMs = 2000);
    ~HealthChecker();

    /// 单次探测完成回调（Probe 内部调用）
    void onProbeResult(const std::string& nodeId, bool ok);

    void start();
    void stop();

    /// 被动记录一次失败/成功（供请求路径调用）
    void recordFailure(const std::string& nodeId);
    void recordSuccess(const std::string& nodeId);

    CircuitStatus status(const std::string& nodeId) const;
    bool allowRequest(const std::string& nodeId) const;

    void setStateChangeCallback(StateChangeCallback cb) {
        onStateChange_ = std::move(cb);
    }

    void setHttpPath(const std::string& path) { healthPath_ = path; }

private:
    void checkAll();
    void probeNode(const NodePtr& node);
    void transition(const std::string& nodeId, CircuitState next);

    EventLoop* loop_;
    NodeRegistry* registry_;
    int intervalSec_;
    int failureThreshold_;
    int cooldownSec_;
    int timeoutMs_;
    std::string healthPath_ = "/api/health";
    bool running_ = false;
    uint64_t timerId_ = 0;

    mutable std::mutex mu_;
    std::map<std::string, CircuitStatus> circuits_;
    StateChangeCallback onStateChange_;

    // 节点 id -> 进行中的探测（用一个简单的计数 id 标识）
    std::map<std::string, uint64_t> inFlight_;
    [[maybe_unused]] uint64_t nextProbeId_ = 1;
};

}  // namespace streaming
