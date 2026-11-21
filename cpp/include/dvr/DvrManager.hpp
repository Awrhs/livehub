#pragma once

#include "core/NonCopyable.hpp"
#include "dvr/Fmp4Recorder.hpp"
#include "net/Timer.hpp"
#include "rtmp/MediaStream.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

class AsyncFileWriter;
class EventLoop;

/**
 * DvrManager: 管理所有流的录制与时移。
 *
 * - startRecord(streamKey): 开始 VOD 录制为单个 fMP4 文件 recordings/<key>.mp4
 * - stopRecord(streamKey):  停止并落盘
 * - 计划录制：scheduleOnce 在 startSec/endSec（epoch 秒）自动启停；
 *            scheduleDaily 在每天的 HH:MM-HH:MM 自动启停（用 64-bit 位图）。
 * - enableTimeshift(key):   开启时移，写入 timeshift/<key>/init.mp4 + seg-N.m4s，
 *                           保留 windowCount 个分片，可回看。
 * - 时移分片通过 HLS-like m3u8 清单做 seek。
 */
class DvrManager : NonCopyable {
public:
    struct Config {
        std::string baseDir = "./dvr";
        uint32_t segmentDurationMs = 6000;
        uint32_t timeshiftWindow = 12;  // 分片数
        bool faststart = false;        // VOD 结束时把 moov 前移
    };

    struct RecordingInfo {
        std::string key;
        std::string path;
        int64_t startEpoch = 0;
        int64_t sizeBytes = 0;
    };

    using ScheduleId = uint64_t;

    DvrManager();
    explicit DvrManager(Config cfg);
    ~DvrManager();

    /// 绑定 EventLoop（计划录制定时任务必须）
    void setLoop(EventLoop* loop);

    /// 注入异步文件写线程
    void setAsyncWriter(AsyncFileWriter* w) { asyncWriter_ = w; }

    /// 获取/创建一路流的时移录制器（幂等）
    std::shared_ptr<Fmp4Recorder> enableTimeshift(
        const std::shared_ptr<MediaStream>& stream);

    /// 开始 VOD 录制；返回录像文件路径
    std::string startRecord(const std::shared_ptr<MediaStream>& stream);
    bool stopRecord(const std::string& streamKey);
    bool isRecording(const std::string& streamKey) const;

    /// 一次性计划录制（epoch 秒）。startSec 之前不会录制。
    ScheduleId scheduleOnce(const std::string& streamKey, int64_t startSec,
                            int64_t endSec,
                            std::function<std::shared_ptr<MediaStream>(
                                const std::string&)> streamResolver);
    /// 每天定时录制（HH:MM 24 小时制，分钟数 0..1439）
    ScheduleId scheduleDaily(const std::string& streamKey,
                             int startMinuteOfDay, int endMinuteOfDay,
                             std::function<std::shared_ptr<MediaStream>(
                                 const std::string&)> streamResolver);
    bool cancelSchedule(ScheduleId id);

    /// 列出磁盘上已完成录像
    std::vector<RecordingInfo> listRecordings() const;

    /// 生成某路流的时移 m3u8（供回看 seek）
    std::string timeshiftPlaylist(const std::string& key) const;

    std::string timeshiftDir(const std::string& key) const {
        return cfg_.baseDir + "/timeshift/" + key;
    }
    std::string recordPath(const std::string& key) const {
        return cfg_.baseDir + "/recordings/" + key + ".mp4";
    }
    const Config& config() const { return cfg_; }

private:
    struct Schedule;
    void tickSchedules();
    void applyFaststart(const std::string& path);

    Config cfg_;
    EventLoop* loop_ = nullptr;
    AsyncFileWriter* asyncWriter_ = nullptr;
    TimerId schedulerTimer_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<Fmp4Recorder>> timeshifts_;
    std::map<std::string, std::shared_ptr<Fmp4Recorder>> records_;
    std::map<ScheduleId, std::shared_ptr<Schedule>> schedules_;
    ScheduleId nextScheduleId_ = 1;
};

}  // namespace streaming
