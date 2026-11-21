#include "cluster/LoadBalancer.hpp"

#include <algorithm>
#include <limits>

namespace streaming {

LoadBalancer::LoadBalancer(NodeRegistry* registry, LbStrategy strategy)
    : registry_(registry), strategy_(strategy) {
    rebuildRing();
}

NodePtr LoadBalancer::selectNode(const std::string& key) {
    auto healthy = registry_->healthy();
    if (healthy.empty()) return nullptr;
    switch (strategy_) {
        case LbStrategy::kRoundRobin:
            return selectRoundRobin(healthy);
        case LbStrategy::kLeastConnections:
            return selectLeast(healthy);
        case LbStrategy::kConsistentHash:
        default:
            return selectHash(healthy, key);
    }
}

NodePtr LoadBalancer::selectRoundRobin(
    const std::vector<NodePtr>& healthy) {
    uint64_t idx = rrCounter_.fetch_add(1, std::memory_order_relaxed);
    return healthy[idx % healthy.size()];
}

NodePtr LoadBalancer::selectLeast(const std::vector<NodePtr>& healthy) {
    NodePtr best;
    int bestLoad = std::numeric_limits<int>::max();
    for (const auto& n : healthy) {
        int l = n->load.load();
        if (l < bestLoad) {
            bestLoad = l;
            best = n;
        }
    }
    return best;
}

NodePtr LoadBalancer::selectHash(const std::vector<NodePtr>& healthy,
                                  const std::string& key) {
    std::string id;
    {
        std::lock_guard<std::mutex> lk(ringMu_);
        id = ring_.getNode(key);
    }
    if (!id.empty()) {
        for (const auto& n : healthy) {
            if (n->id == id) return n;
        }
    }
    // 环上节点已不健康：回退到最少连接
    return selectLeast(healthy);
}

void LoadBalancer::reportLoad(const NodePtr& node) {
    if (node) node->load.fetch_add(1, std::memory_order_relaxed);
}

void LoadBalancer::releaseNode(const NodePtr& node) {
    if (node) {
        node->load.fetch_sub(1, std::memory_order_relaxed);
    }
}

void LoadBalancer::rebuildRing() {
    std::lock_guard<std::mutex> lk(ringMu_);
    ring_.clear();
    for (const auto& n : registry_->all()) {
        ring_.addNode(n->id);
    }
}

}  // namespace streaming
