#include "cache/LruCache.hpp"
#include "test_framework.hpp"

#include <string>
#include <thread>

using namespace streaming;

TEST(lru_eviction_order) {
    LruCache<int, std::string> cache(3);
    cache.put(1, "a");
    cache.put(2, "b");
    cache.put(3, "c");
    cache.get(1);          // 1 变最近使用
    cache.put(4, "d");     // 淘汰 2
    ASSERT_EQ(cache.get(2).get(), nullptr);
    ASSERT_TRUE(cache.get(1) != nullptr);
}

TEST(lru_hit_miss) {
    LruCache<int, int> c(4);
    c.put(1, 10);
    ASSERT_TRUE(c.get(1) != nullptr);
    ASSERT_EQ(*c.get(1), 10);
    ASSERT_TRUE(c.get(99) == nullptr);
}

TEST(lru_size_cap) {
    LruCache<int, int> c(2);
    c.put(1, 1);
    c.put(2, 2);
    c.put(3, 3);
    ASSERT_EQ(c.size(), size_t(2));
}

TEST(sharded_lru_concurrent) {
    ShardedLruCache<int, int, 8> c(1000);
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&c, t] {
            for (int i = 0; i < 1000; ++i) {
                c.put(t * 10000 + i, i);
                c.get(t * 10000 + i);
            }
        });
    }
    for (auto& th : ts) th.join();
    ASSERT_TRUE(c.size() > 0);
}
