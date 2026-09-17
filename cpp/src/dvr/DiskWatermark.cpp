#include "dvr/DiskWatermark.hpp"

#include "core/Logger.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>

namespace streaming {

DiskWatermark::DiskWatermark(std::string path, int highPercent,
                             int lowPercent)
    : path_(std::move(path)),
      high_(highPercent),
      low_(lowPercent) {}

void DiskWatermark::addManagedDir(std::string dir, std::string suffix) {
    dirs_.emplace_back(std::move(dir), std::move(suffix));
}

DiskWatermark::Stat DiskWatermark::stat() const {
    Stat s;
    struct statvfs vfs;
    if (::statvfs(path_.c_str(), &vfs) != 0) return s;
    s.totalBytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    s.availBytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    return s;
}

// 删除某个目录下最旧的一个匹配后缀文件，返回 true 表示删除了一个
static bool deleteOldestFile(const std::string& dir,
                             const std::string& suffix) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return false;

    std::string oldest;
    time_t oldestMtime = 0;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (!suffix.empty() &&
            name.size() <= suffix.size())
            continue;
        if (!suffix.empty() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (oldest.empty() || st.st_mtime < oldestMtime) {
            oldest = full;
            oldestMtime = st.st_mtime;
        }
    }
    ::closedir(d);

    if (oldest.empty()) return false;
    if (::unlink(oldest.c_str()) == 0) {
        LOG_WARN("DiskWatermark: removed oldest file %s", oldest.c_str());
        return true;
    }
    return false;
}

bool DiskWatermark::check() {
    Stat s = stat();
    int usage = s.usagePercent();
    if (usage < high_) return true;

    LOG_WARN("DiskWatermark: usage %d%% >= high %d%%, cleaning up", usage,
             high_);

    // 优先用自定义清理回调
    if (cleanupCb_) {
        cleanupCb_();
    }

    // 反复删最旧文件，直到降到 low 水位或无文件可删
    while (s.usagePercent() > low_) {
        bool removed = false;
        for (const auto& kv : dirs_) {
            if (deleteOldestFile(kv.first, kv.second)) {
                removed = true;
                break;
            }
        }
        if (!removed) {
            LOG_ERROR("DiskWatermark: nothing to delete, usage %d%%",
                      s.usagePercent());
            return false;
        }
        s = stat();
    }

    LOG_INFO("DiskWatermark: cleaned, usage now %d%%", s.usagePercent());
    return true;
}

}  // namespace streaming
