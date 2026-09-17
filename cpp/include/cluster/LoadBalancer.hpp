#pragma once

#include "cluster/ConsistentHash.hpp"
#include "cluster/NodeRegistry.hpp"
#include "core/NonCopyable.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

/**
 * LoadBalanceStrategy: 负载均衡策略。
 */
enum class LbStrategy { kRoundRobin, kLeastConnections, kConsistentHash };

/**
 * LoadBalancer: 在健康节点间选择。
 *
 * - RoundRobin：轮询。
 * - LeastConnections：选择 load 最小的节点。
 * - ConsistentHash：按 stream key 一致性哈希，同流亲和同节点。
 *
 * reportLoad / releaseNode 用于维护各节点连接计数。
 */
class LoadBalancer : NonCopyable {
public:
    explicit LoadBalancer(NodeRegistry* registry,
                           LbStrategy strategy = LbStrategy::kConsistentHash);

    void setStrategy(LbStrategy s) { strategy_ = s; }
    LbStrategy strategy() const { return strategy_; }

    /// 选择节点；无健康节点返回 nullptr
    NodePtr selectNode(const std::string& key = "");

    /// 节点被选中后报告负载 +1
    void reportLoad(const NodePtr& node);
    /// 连接释放后负载 -1
    void releaseNode(const NodePtr& node);

    /// 当节点增减时同步一致性哈希环
    void rebuildRing();

private:
    NodePtr selectRoundRobin(const std::vector<NodePtr>& healthy);
    NodePtr selectLeast(const std::vector<NodePtr>& healthy);
    NodePtr selectHash(const std::vector<NodePtr>& healthy,
                       const std::string& key);

    NodeRegistry* registry_;
    LbStrategy strategy_;
    ConsistentHash ring_;
    std::atomic<uint64_t> rrCounter_{0};
    std::mutex ringMu_;
};

}  // namespace streaming
