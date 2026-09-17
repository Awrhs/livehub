#include "cluster/NodeRegistry.hpp"

namespace streaming {

void NodeRegistry::add(const NodePtr& node) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    nodes_[node->id] = node;
}

bool NodeRegistry::remove(const std::string& id) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    return nodes_.erase(id) > 0;
}

NodePtr NodeRegistry::get(const std::string& id) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second;
}

std::vector<NodePtr> NodeRegistry::all() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<NodePtr> out;
    out.reserve(nodes_.size());
    for (const auto& [id, n] : nodes_) out.push_back(n);
    return out;
}

std::vector<NodePtr> NodeRegistry::healthy() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<NodePtr> out;
    for (const auto& [id, n] : nodes_) {
        if (n->healthy.load()) out.push_back(n);
    }
    return out;
}

size_t NodeRegistry::size() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return nodes_.size();
}

void NodeRegistry::clear() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    nodes_.clear();
}

}  // namespace streaming
