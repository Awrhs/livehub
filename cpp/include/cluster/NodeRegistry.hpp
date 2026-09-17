#pragma once

#include "cluster/Node.hpp"
#include "core/NonCopyable.hpp"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace streaming {

/**
 * NodeRegistry: 线程安全的节点注册表（读写锁）。
 */
class NodeRegistry : NonCopyable {
public:
    void add(const NodePtr& node);
    bool remove(const std::string& id);
    NodePtr get(const std::string& id) const;
    std::vector<NodePtr> all() const;
    std::vector<NodePtr> healthy() const;
    size_t size() const;
    void clear();

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, NodePtr> nodes_;
};

}  // namespace streaming
