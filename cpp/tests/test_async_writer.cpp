#include "core/AsyncFileWriter.hpp"
#include "test_framework.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace streaming;

namespace {
bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
std::string tmpPath(const char* name) {
    return "/tmp/aw_test_" + std::to_string(::getpid()) + "_" + name;
}
// 等待 close 回调
struct Waiter {
    std::atomic<int> done{0};
    std::atomic<bool> ok{true};
    void wait() {
        for (int i = 0; i < 500 && done.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
};
}  // namespace

TEST(async_writer_complete_file_kept) {
    AsyncFileWriter w(1 << 20);
    w.start();
    std::string p = tmpPath("ok");
    uint64_t id = w.open(p, false, true);
    std::vector<uint8_t> data(1000, 7);
    w.append(id, data);
    Waiter wt;
    w.close(id, [&](bool ok, uint64_t n) {
        wt.ok = ok && n == 1000;
        wt.done = 1;
    });
    wt.wait();
    ASSERT_TRUE(wt.done.load() == 1);
    ASSERT_TRUE(wt.ok.load());
    ASSERT_TRUE(exists(p));
    w.stop();
    ::unlink(p.c_str());
}

TEST(async_writer_incomplete_file_discarded_when_queue_full) {
    // 队列上限 4KB：一次性塞入远超上限的数据，必有写入被丢弃
    AsyncFileWriter w(4096);
    std::string p = tmpPath("dropped");
    uint64_t id = w.open(p, false, /*discardIfIncomplete=*/true);
    std::vector<uint8_t> chunk(3000, 1);
    for (int i = 0; i < 10; ++i) w.append(id, chunk);  // 写线程尚未启动，队列持续积压
    ASSERT_TRUE(w.droppedBytes() > 0);
    Waiter wt;
    w.close(id, [&](bool ok, uint64_t) {
        wt.ok = ok;
        wt.done = 1;
    });
    w.start();
    wt.wait();
    ASSERT_TRUE(wt.done.load() == 1);
    ASSERT_TRUE(!wt.ok.load());  // 回调报告失败
    ASSERT_TRUE(!exists(p));     // 不完整的文件被删除
    w.stop();
}

TEST(async_writer_incomplete_file_kept_without_discard) {
    AsyncFileWriter w(4096);
    std::string p = tmpPath("kept");
    uint64_t id = w.open(p, false, /*discardIfIncomplete=*/false);
    std::vector<uint8_t> chunk(3000, 1);
    for (int i = 0; i < 10; ++i) w.append(id, chunk);
    Waiter wt;
    w.close(id, [&](bool ok, uint64_t) {
        wt.ok = ok;
        wt.done = 1;
    });
    w.start();
    wt.wait();
    ASSERT_TRUE(!wt.ok.load());
    ASSERT_TRUE(exists(p));  // 长录像：保留已写部分
    w.stop();
    ::unlink(p.c_str());
}
