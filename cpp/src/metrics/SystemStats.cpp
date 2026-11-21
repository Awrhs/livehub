#include "metrics/SystemStats.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace streaming {

SystemStats& SystemStats::instance() {
    static SystemStats s;
    return s;
}

bool SystemStats::parseCpuStatLine(const std::string& line, CpuSample* out) {
    if (!out) return false;
    // 形如 "cpu  12345 678 90123 4567890 123 456 789 12"
    std::istringstream is(line);
    std::string cpu;
    uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0,
             softirq = 0, steal = 0;
    if (!(is >> cpu >> user >> nice >> sys >> idle >> iowait >> irq >>
          softirq >> steal)) {
        return false;
    }
    out->idle = idle + iowait;
    out->total = user + nice + sys + idle + iowait + irq + softirq + steal;
    return true;
}

double SystemStats::deltaUsage(const CpuSample& prev, const CpuSample& now) {
    uint64_t totalDelta = now.total - prev.total;
    uint64_t idleDelta = now.idle - prev.idle;
    // 计数回绕（极罕见）或无推进时按 0 处理
    if (totalDelta == 0 || now.total < prev.total) return 0.0;
    if (now.idle < prev.idle) idleDelta = 0;
    return 1.0 - static_cast<double>(idleDelta) /
                     static_cast<double>(totalDelta);
}

bool SystemStats::parseMemInfo(const std::string& text, double* usedRatio) {
    uint64_t memTotal = 0, memAvailable = 0;
    bool haveTotal = false, haveAvail = false;
    std::istringstream is(text);
    std::string key;
    uint64_t kb;
    std::string unit;
    while (is >> key >> kb >> unit) {
        if (key == "MemTotal:") { memTotal = kb; haveTotal = true; }
        else if (key == "MemAvailable:") { memAvailable = kb; haveAvail = true; }
        if (haveTotal && haveAvail) break;
    }
    // 缺任一字段都视为解析失败（否则会误报内存 100% 已用）
    if (!haveTotal || !haveAvail || memTotal == 0 || memAvailable > memTotal)
        return false;
    if (usedRatio) {
        *usedRatio = 1.0 - static_cast<double>(memAvailable) /
                                static_cast<double>(memTotal);
    }
    return true;
}

double SystemStats::cpuUsage() {
    std::ifstream f("/proc/stat");
    if (!f) return 0.0;
    std::string line;
    std::getline(f, line);
    CpuSample now;
    if (!parseCpuStatLine(line, &now)) return 0.0;
    std::lock_guard<std::mutex> lk(mu_);
    double usage = 0.0;
    if (hasLastCpu_) usage = deltaUsage(lastCpu_, now);
    lastCpu_ = now;
    hasLastCpu_ = true;
    return usage;
}

double SystemStats::memUsage() {
    std::ifstream f("/proc/meminfo");
    if (!f) return -1.0;
    std::stringstream ss;
    ss << f.rdbuf();
    double ratio = 0.0;
    if (!parseMemInfo(ss.str(), &ratio)) return -1.0;
    return ratio;
}

double SystemStats::loadAvg1() {
    std::ifstream f("/proc/loadavg");
    if (!f) return -1.0;
    double load1 = 0.0;
    if (!(f >> load1)) return -1.0;
    return load1;
}

int64_t SystemStats::uptimeSec() {
    std::ifstream f("/proc/uptime");
    if (!f) return -1;
    double up = 0.0, idle = 0.0;
    if (!(f >> up >> idle)) return -1;
    (void)idle;
    return static_cast<int64_t>(up);
}

}  // namespace streaming
