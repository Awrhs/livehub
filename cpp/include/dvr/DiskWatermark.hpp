#pragma once

#include "core/NonCopyable.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace streaming {

/**
 * DiskWatermark: 磁盘水位保护。
 *
 * 周期检查 DVR/HLS 所在文件系统的使用率，超过 high 水位时回调清理逻辑
 * （通常删除最旧的录像），直到使用率降到 low 水位以下或无文件可删。
 * 防止磁盘写满导致直播/录像崩溃。
 */
class DiskWatermark : NonCopyable {
public:
    /// 检查结果
    struct Stat {
        uint64_t totalBytes = 0;
        uint64_t availBytes = 0;
        int usagePercent() const {
            if (totalBytes == 0) return 0;
            return static_cast<int>(
                100 - (availBytes * 100 / totalBytes));
        }
    };

    using CleanupCallback = std::function<int()>;  // 返回删除的文件数

    DiskWatermark(std::string path, int highPercent = 90,
                  int lowPercent = 80);

    /// 设置需要保活的目录与清理回调（按修改时间删最旧）
    void addManagedDir(std::string dir, std::string suffix = ".mp4");
    void setCleanupCallback(CleanupCallback cb) { cleanupCb_ = std::move(cb); }

    /// 立即检查一次；返回是否安全（低于 high 或已清理到 low）
    bool check();

    /// 获取当前统计
    Stat stat() const;

    int highPercent() const { return high_; }
    int lowPercent() const { return low_; }

private:
    std::string path_;
    int high_;
    int low_;
    std::vector<std::pair<std::string, std::string>> dirs_;
    CleanupCallback cleanupCb_;
};

}  // namespace streaming
