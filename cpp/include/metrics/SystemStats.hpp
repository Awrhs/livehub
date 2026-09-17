#pragma once

#include "core/NonCopyable.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace streaming {

/**
 * SystemStats: 从 /proc 读取系统级指标（Linux）。
 *
 * - cpuUsage：/proc/stat 首行差分采样（两次调用之间占用比），内部保存
 *   上次采样值；首次调用返回 0。
 * - memUsage：/proc/meminfo 的 1 - MemAvailable/MemTotal。
 * - loadAvg1 / uptimeSec：/proc/loadavg、/proc/uptime。
 *
 * 解析函数拆成静态纯函数（吃字符串），便于单元测试注入。
 */
class SystemStats : NonCopyable {
public:
    static SystemStats& instance();

    /// CPU 占用比 0..1（差分；首次调用返回 0）
    double cpuUsage();

    /// 内存占用比 0..1（读取失败返回 -1）
    double memUsage();

    /// 1 分钟负载（读取失败返回 -1）
    double loadAvg1();

    /// 系统已运行秒数（读取失败返回 -1）
    int64_t uptimeSec();

    // ---- 纯解析函数（供测试与复用）----

    struct CpuSample {
        uint64_t idle = 0;   // idle + iowait
        uint64_t total = 0;  // user+nice+system+idle+iowait+irq+softirq+steal
    };

    /// 解析 /proc/stat 首行 "cpu  u n s i w irq sirq steal ..."
    static bool parseCpuStatLine(const std::string& line, CpuSample* out);

    /// 两次采样之间的 CPU 占用比；total 差为 0（首次/时钟未走）返回 0
    static double deltaUsage(const CpuSample& prev, const CpuSample& now);

    /// 解析 /proc/meminfo 全文，输出占用比；缺 MemTotal/MemAvailable 返回 false
    static bool parseMemInfo(const std::string& text, double* usedRatio);

private:
    SystemStats() = default;

    std::mutex mu_;
    CpuSample lastCpu_;
    bool hasLastCpu_ = false;
};

}  // namespace streaming
