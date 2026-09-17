#include "cluster/ConsistentHash.hpp"
#include "test_framework.hpp"

#include <set>
#include <string>

using namespace streaming;

TEST(chash_empty_ring) {
    ConsistentHash ring(100);
    ASSERT_EQ(ring.nodeCount(), size_t(0));
    ASSERT_TRUE(ring.getNode("any").empty());
}

TEST(chash_add_lookup) {
    ConsistentHash ring(150);
    ring.addNode("node-A");
    ring.addNode("node-B");
    ring.addNode("node-C");
    ASSERT_EQ(ring.nodeCount(), size_t(3));
    ASSERT_EQ(ring.ringSize(), size_t(450));

    // 同 key 稳定映射
    std::string first = ring.getNode("stream-1");
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(ring.getNode("stream-1"), first);
    }
    ASSERT_FALSE(first.empty());
}

TEST(chash_distribution_even) {
    ConsistentHash ring(150);
    for (int i = 0; i < 10; ++i) {
        ring.addNode("node-" + std::to_string(i));
    }
    std::vector<std::string> keys;
    for (int i = 0; i < 10000; ++i) {
        keys.push_back("key-" + std::to_string(i));
    }
    auto dist = ring.distribution(keys);
    ASSERT_EQ(dist.size(), size_t(10));
    // 每节点应在 1000 ± 400 之间（宽松，避免 MD5 抖动误报）
    for (const auto& kv : dist) {
        ASSERT_GT(kv.second, 600);
        ASSERT_LT(kv.second, 1400);
    }
}

TEST(chash_remove_minimal_migration) {
    ConsistentHash ring(200);
    for (int i = 0; i < 8; ++i) ring.addNode("n" + std::to_string(i));

    std::vector<std::string> keys;
    for (int i = 0; i < 5000; ++i) keys.push_back("k" + std::to_string(i));

    std::map<std::string, std::string> before;
    for (const auto& k : keys) before[k] = ring.getNode(k);

    ring.removeNode("n3");

    int migrated = 0;
    for (const auto& k : keys) {
        if (ring.getNode(k) != before[k]) ++migrated;
    }
    // 移除 1/8 节点，迁移应接近 1/8（12.5%），远小于全量重映射
    double ratio = double(migrated) / keys.size();
    ASSERT_GT(ratio, 0.05);
    ASSERT_LT(ratio, 0.30);
}

TEST(chash_md5_known_vector) {
    // 空串 MD5 = d41d8cd98f00b204e9800998ecf8427e
    std::string h = ConsistentHash::md5("");
    ASSERT_EQ(h.size(), size_t(16));
    ASSERT_EQ(uint8_t(h[0]), uint8_t(0xd4));
    ASSERT_EQ(uint8_t(h[1]), uint8_t(0x1d));
    ASSERT_EQ(uint8_t(h[2]), uint8_t(0x8c));
    ASSERT_EQ(uint8_t(h[3]), uint8_t(0xd9));
}
