#include "dvr/DvrManager.hpp"

#include "core/AsyncFileWriter.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "dvr/Mp4Faststart.hpp"
#include "net/EventLoop.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <dirent.h>

namespace streaming {

namespace {
void mkdirp(const std::string& path) {
    if (path.empty()) return;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size())
            ::mkdir(cur.c_str(), 0755);
    }
}
}  // namespace

// 计划任务内部状态
struct DvrManager::Schedule {
    enum Type { kOnce, kDaily };
    Type type = kOnce;
    std::string key;
    int64_t startSec = 0;
    int64_t endSec = 0;
    int startMinute = 0;   // daily
    int endMinute = 0;     // daily
    bool active = false;   // 当前是否已在录制
    std::function<std::shared_ptr<MediaStream>(const std::string&)> resolver;
};

DvrManager::DvrManager() : DvrManager(Config{}) {}

DvrManager::DvrManager(Config cfg) : cfg_(std::move(cfg)) {
    mkdirp(cfg_.baseDir);
    mkdirp(cfg_.baseDir + "/timeshift");
    mkdirp(cfg_.baseDir + "/recordings");
}

DvrManager::~DvrManager() {
    if (loop_ && schedulerTimer_) {
        loop_->cancel(schedulerTimer_);
    }
}

void DvrManager::setLoop(EventLoop* loop) {
    loop_ = loop;
    if (loop_ && !schedulerTimer_) {
        // 每 30 秒检查一次计划录制
        schedulerTimer_ =
            loop_->runEvery(30.0, [this] { tickSchedules(); });
    }
}

std::shared_ptr<Fmp4Recorder> DvrManager::enableTimeshift(
    const std::shared_ptr<MediaStream>& stream) {
    if (!stream) return nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = timeshifts_.find(stream->key());
    if (it != timeshifts_.end()) return it->second;

    Fmp4Recorder::Options opt;
    opt.outputDir = timeshiftDir(stream->key());
    opt.segmentDurationMs = cfg_.segmentDurationMs;
    opt.windowCount = cfg_.timeshiftWindow;
    opt.vodMode = false;
    auto rec = std::make_shared<Fmp4Recorder>(stream, opt);
    if (asyncWriter_) rec->setAsyncWriter(asyncWriter_);
    rec->start();
    timeshifts_[stream->key()] = rec;
    LOG_INFO("DVR: timeshift enabled for %s", stream->key().c_str());
    return rec;
}

std::string DvrManager::startRecord(
    const std::shared_ptr<MediaStream>& stream) {
    if (!stream) return "";
    std::lock_guard<std::mutex> lk(mu_);
    if (records_.count(stream->key())) return recordPath(stream->key());

    Fmp4Recorder::Options opt;
    opt.outputDir = recordPath(stream->key());
    opt.vodMode = true;
    auto rec = std::make_shared<Fmp4Recorder>(stream, opt);
    if (asyncWriter_) rec->setAsyncWriter(asyncWriter_);
    rec->start();
    records_[stream->key()] = rec;
    LOG_INFO("DVR: recording started for %s -> %s", stream->key().c_str(),
             opt.outputDir.c_str());
    return opt.outputDir;
}

bool DvrManager::stopRecord(const std::string& streamKey) {
    std::shared_ptr<Fmp4Recorder> rec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = records_.find(streamKey);
        if (it == records_.end()) return false;
        rec = it->second;
        records_.erase(it);
    }
    rec->stop();
    if (cfg_.faststart) applyFaststart(recordPath(streamKey));
    return true;
}

bool DvrManager::isRecording(const std::string& streamKey) const {
    std::lock_guard<std::mutex> lk(mu_);
    return records_.count(streamKey) > 0;
}

DvrManager::ScheduleId DvrManager::scheduleOnce(
    const std::string& streamKey, int64_t startSec, int64_t endSec,
    std::function<std::shared_ptr<MediaStream>(const std::string&)> resolver) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = std::make_shared<Schedule>();
    s->type = Schedule::kOnce;
    s->key = streamKey;
    s->startSec = startSec;
    s->endSec = endSec;
    s->resolver = std::move(resolver);
    ScheduleId id = nextScheduleId_++;
    schedules_[id] = s;
    LOG_INFO("DVR: schedule once %s [%lld..%lld]", streamKey.c_str(),
             static_cast<long long>(startSec),
             static_cast<long long>(endSec));
    return id;
}

DvrManager::ScheduleId DvrManager::scheduleDaily(
    const std::string& streamKey, int startMinuteOfDay, int endMinuteOfDay,
    std::function<std::shared_ptr<MediaStream>(const std::string&)> resolver) {
    std::lock_guard<std::mutex> lk(mu_);
    auto s = std::make_shared<Schedule>();
    s->type = Schedule::kDaily;
    s->key = streamKey;
    s->startMinute = startMinuteOfDay;
    s->endMinute = endMinuteOfDay;
    s->resolver = std::move(resolver);
    ScheduleId id = nextScheduleId_++;
    schedules_[id] = s;
    LOG_INFO("DVR: schedule daily %s %02d:%02d-%02d:%02d",
             streamKey.c_str(), startMinuteOfDay / 60,
             startMinuteOfDay % 60, endMinuteOfDay / 60, endMinuteOfDay % 60);
    return id;
}

bool DvrManager::cancelSchedule(ScheduleId id) {
    bool active = false;
    std::string key;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = schedules_.find(id);
        if (it == schedules_.end()) return false;
        active = it->second->active;
        key = it->second->key;
        schedules_.erase(it);
    }
    if (active) stopRecord(key);
    return true;
}

void DvrManager::tickSchedules() {
    int64_t now = Timestamp::now().secondsSinceEpoch();
    time_t t = static_cast<time_t>(now);
    struct tm gmt;
    gmtime_r(&t, &gmt);
    int minuteOfDay = gmt.tm_hour * 60 + gmt.tm_min;

    std::vector<std::pair<std::string, bool>> actions;  // key, start?
    std::vector<ScheduleId> expired;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : schedules_) {
            auto& s = kv.second;
            if (s->type == Schedule::kOnce) {
                if (!s->active && now >= s->startSec && now < s->endSec) {
                    actions.emplace_back(s->key, true);
                    s->active = true;
                } else if (s->active && now >= s->endSec) {
                    actions.emplace_back(s->key, false);
                    s->active = false;
                    expired.push_back(kv.first);
                } else if (!s->active && now >= s->endSec) {
                    expired.push_back(kv.first);  // 从未开始即过期
                }
            } else {  // daily
                bool inWindow = minuteOfDay >= s->startMinute &&
                                minuteOfDay < s->endMinute;
                if (inWindow && !s->active) {
                    actions.emplace_back(s->key, true);
                    s->active = true;
                } else if (!inWindow && s->active) {
                    actions.emplace_back(s->key, false);
                    s->active = false;
                }
            }
        }
        for (ScheduleId id : expired) schedules_.erase(id);
    }

    for (const auto& a : actions) {
        // 找到该 key 对应的 schedule 以拿 resolver
        std::shared_ptr<Schedule> s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& kv : schedules_)
                if (kv.second->key == a.first) {
                    s = kv.second;
                    break;
                }
        }
        if (a.second) {
            std::shared_ptr<MediaStream> stream;
            if (s && s->resolver) stream = s->resolver(a.first);
            if (stream) startRecord(stream);
        } else {
            stopRecord(a.first);
        }
    }
}

void DvrManager::applyFaststart(const std::string& path) {
    Mp4Faststart::relocate(path);
}

std::vector<DvrManager::RecordingInfo> DvrManager::listRecordings() const {
    std::vector<RecordingInfo> out;
    std::string dir = cfg_.baseDir + "/recordings";
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (name.size() < 4 ||
            name.compare(name.size() - 4, 4, ".mp4") != 0)
            continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) continue;
        RecordingInfo info;
        info.key = name.substr(0, name.size() - 4);
        info.path = full;
        info.startEpoch = static_cast<int64_t>(st.st_mtime);
        info.sizeBytes = static_cast<int64_t>(st.st_size);
        out.push_back(std::move(info));
    }
    ::closedir(d);
    return out;
}

std::string DvrManager::timeshiftPlaylist(const std::string& key) const {
    std::string dir = timeshiftDir(key);
    std::vector<uint32_t> seqs;
    DIR* d = ::opendir(dir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = ::readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            // seg-N.m4s
            if (name.rfind("seg-", 0) != 0) continue;
            auto dash = name.find('-', 4);
            auto dot = name.find('.', dash);
            if (dash == std::string::npos || dot == std::string::npos)
                continue;
            seqs.push_back(static_cast<uint32_t>(
                std::stoul(name.substr(dash + 1, dot - dash - 1))));
        }
        ::closedir(d);
    }
    std::sort(seqs.begin(), seqs.end());

    std::string pl = "#EXTM3U\n#EXT-X-VERSION:7\n";
    pl += "#EXT-X-PLAYLIST-TYPE:EVENT\n";
    pl += "#EXT-X-TARGETDURATION:" +
          std::to_string((cfg_.segmentDurationMs + 999) / 1000) + "\n";
    if (!seqs.empty()) pl += "#EXT-X-MEDIA-SEQUENCE:" +
                             std::to_string(seqs.front()) + "\n";
    pl += "#EXT-X-MAP:URI=\"init.mp4\"\n";
    double approxSec = cfg_.segmentDurationMs / 1000.0;
    char buf[16];
    for (uint32_t s : seqs) {
        std::snprintf(buf, sizeof(buf), "%.3f", approxSec);
        pl += "#EXTINF:";
        pl += buf;
        pl += ",\nseg-";
        pl += std::to_string(s);
        pl += ".m4s\n";
    }
    return pl;
}

}  // namespace streaming
