#include "metrics/SystemStats.hpp"
#include "test_framework.hpp"

#include <string>

using namespace streaming;

// /proc 格式解析（注入字符串，不依赖真机读数）

TEST(system_stats_cpu_line_parse) {
    SystemStats::CpuSample s;
    ASSERT_TRUE(SystemStats::parseCpuStatLine(
        "cpu  100 20 30 7000 100 10 5 2", &s));
    // idle = 7000 + 100(iowait) = 7100
    ASSERT_EQ(s.idle, uint64_t(7100));
    // total = 100+20+30+7000+100+10+5+2 = 7267
    ASSERT_EQ(s.total, uint64_t(7267));

    // 字段不足 → 解析失败
    ASSERT_FALSE(SystemStats::parseCpuStatLine("cpu  100 20", &s));
    ASSERT_FALSE(SystemStats::parseCpuStatLine("intr 12345", &s));
}

TEST(system_stats_cpu_delta) {
    SystemStats::CpuSample a, b;
    a.idle = 1000;
    a.total = 2000;
    b.idle = 1500;
    b.total = 3000;
    // delta: idle +500 / total +1000 → 占用 50%
    double u = SystemStats::deltaUsage(a, b);
    ASSERT_TRUE(u > 0.49 && u < 0.51);

    // 无推进（total 相同）→ 0
    ASSERT_EQ(SystemStats::deltaUsage(a, a), 0.0);

    // 计数回绕（now < prev）→ 0，不得出现负值/除零
    SystemStats::CpuSample wrap;
    wrap.idle = 5;
    wrap.total = 5;
    ASSERT_EQ(SystemStats::deltaUsage(a, wrap), 0.0);
}

TEST(system_stats_meminfo_parse) {
    std::string text =
        "MemTotal:       8000000 kB\n"
        "MemFree:         100000 kB\n"
        "MemAvailable:   2000000 kB\n"
        "Buffers:          50000 kB\n";
    double ratio = -1;
    ASSERT_TRUE(SystemStats::parseMemInfo(text, &ratio));
    // 1 - 2000000/8000000 = 0.75
    ASSERT_TRUE(ratio > 0.74 && ratio < 0.76);

    // 缺 MemAvailable → 失败
    ASSERT_FALSE(SystemStats::parseMemInfo("MemTotal: 100 kB\n", &ratio));
    // 空文本 → 失败
    ASSERT_FALSE(SystemStats::parseMemInfo("", &ratio));
}

TEST(system_stats_instance_reads_proc) {
    // 在真机（CI 容器）上 /proc 必然存在；只断言值域，不断言具体数值。
    // cpuUsage 首次调用返回 0（无上次采样）。
    ASSERT_EQ(SystemStats::instance().cpuUsage(), 0.0);
    double mem = SystemStats::instance().memUsage();
    ASSERT_TRUE(mem == -1.0 || (mem >= 0.0 && mem <= 1.0));
}
