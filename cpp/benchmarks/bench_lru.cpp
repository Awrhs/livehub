// LRU 缓存命中率与并发基准。
#include "cache/LruCache.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using namespace streaming;

static void benchSingleThread(int ops, int range) {
    LruCache<int, int> cache(range / 2);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, range - 1);
    int hits = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ops; ++i) {
        int k = dist(rng);
        if (cache.get(k).get()) {
            ++hits;
        } else {
            cache.put(k, k);
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("LRU single-thread: %d ops, %.2f Mops/s, hit %.1f%%, %.3f sec\n",
                ops, ops / sec / 1e6, 100.0 * hits / ops, sec);
}

static void benchSharded(int ops, int threads) {
    ShardedLruCache<int, int, 16> cache(100000);
    std::vector<std::thread> ts;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            std::mt19937 rng(t);
            std::uniform_int_distribution<int> dist(0, 200000 - 1);
            for (int i = 0; i < ops; ++i) {
                int k = dist(rng);
                if (!cache.get(k)) cache.put(k, k);
            }
        });
    }
    for (auto& th : ts) th.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    long long total = (long long)ops * threads;
    std::printf("ShardedLRU %d threads: %lld ops, %.2f Mops/s, %.3f sec\n",
                threads, total, total / sec / 1e6, sec);
}

int main(int argc, char** argv) {
    int ops = argc > 1 ? std::atoi(argv[1]) : 5'000'000;
    benchSingleThread(ops, 100000);
    benchSharded(ops / 4, 4);
    benchSharded(ops / 8, 8);
    return 0;
}
