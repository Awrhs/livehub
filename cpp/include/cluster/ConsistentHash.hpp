#pragma once

#include "core/NonCopyable.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

/**
 * ConsistentHash: 一致性哈希环。
 *
 * - key 经 MD5 取前 4 字节为 uint32 环上点。
 * - 每个物理节点 VNODE_NUM（默认 150）个虚拟节点，分布均匀。
 * - upper_bound 找后继；节点增减时最小化 key 迁移。
 *
 * 线程安全：内部加锁。
 */
class ConsistentHash : NonCopyable {
public:
    explicit ConsistentHash(int vnodes = 150) : vnodes_(vnodes) {}

    void addNode(const std::string& node);
    void removeNode(const std::string& node);
    void clear();

    /// 返回 key 负责的节点；空环返回空串
    std::string getNode(const std::string& key) const;

    /// 统计 key 到各节点的分布（均匀性测试用）
    std::map<std::string, int> distribution(
        const std::vector<std::string>& keys) const;

    size_t nodeCount() const;
    size_t ringSize() const;

    /// 环上点 → 节点名的快照（测试/诊断用）
    std::map<uint32_t, std::string> snapshot() const;

    /// MD5（16 字节），用于哈希（自包含实现，无 OpenSSL 依赖）
    static std::string md5(const std::string& input);

private:
    static uint32_t hashKey(const std::string& key);
    mutable std::mutex mu_;
    int vnodes_;
    std::map<uint32_t, std::string> ring_;
};

}  // namespace streaming
