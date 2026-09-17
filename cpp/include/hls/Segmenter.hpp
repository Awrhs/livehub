#pragma once

#include "core/NonCopyable.hpp"
#include "hls/M3u8Playlist.hpp"
#include "hls/TsMuxer.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace streaming {

class AsyncFileWriter;

/**
 * Segmenter: 按 GOP / 时长把 TS 流切片成 .ts 文件并维护 m3u8。
 *
 * 设计要点：
 * - 持有 TsMuxer，所有写出的 TS 字节写入当前分片文件
 * - 在关键帧处调用 newSegment() 滚动分片（调用方通知）
 * - 达到目标分片数后淘汰旧分片文件
 * - 线程安全：所有调用应在同一 IO loop / strand
 *
 * 简历亮点：HLS 切片、文件滚动、m3u8 动态维护、旧分片回收
 */
class Segmenter : NonCopyable {
public:
    using SegmentCompleteCallback =
        std::function<void(uint64_t sequence, const std::string& path)>;

    Segmenter(std::string outputDir, std::string streamName,
              int targetDuration = 6, int windowSize = 5);
    ~Segmenter();

    /// 打开/初始化
    bool open();

    /// 注入异步文件写线程（可选）。设置后所有分片写盘走后台线程，
    /// 不阻塞 Reactor。不设置则回退为同步 std::ofstream。
    void setAsyncWriter(AsyncFileWriter* w) { asyncWriter_ = w; }

    /// 设置 playlist 类型（live/event/vod）。event/vod 不删除旧分片。
    void setPlaylistType(PlaylistType t) { playlist_.setType(t); playlistType_ = t; }

    /// 获取底层 muxer（写入音视频帧）
    TsMuxer& muxer() { return *muxer_; }

    /// 在关键帧处滚动到新分片。
    /// @param boundaryPts 新分片首帧的 90kHz PTS（也即上一分片的结束时刻）。
    void newSegment(int64_t boundaryPts, bool discontinuity = false);

    /// 通知当前分片已结束（流结束）
    void finalize();

    /// 当前 m3u8 内容
    std::string playlist();

    std::string segmentPath(uint64_t sequence) const;
    uint64_t currentSequence() const { return sequence_; }
    std::string outputDir() const { return outputDir_; }
    std::string streamName() const { return streamName_; }

    void setSegmentCompleteCallback(SegmentCompleteCallback cb) {
        onSegmentComplete_ = std::move(cb);
    }

private:
    void writeTs(const uint8_t* data, size_t len);
    void closeCurrentSegment(int64_t endPts);
    void deleteOldSegments();

    std::string outputDir_;
    std::string streamName_;
    int targetDuration_;
    int windowSize_;

    std::unique_ptr<TsMuxer> muxer_;
    M3u8Playlist playlist_;
    PlaylistType playlistType_ = PlaylistType::kLive;

    std::ofstream currentFile_;          // 同步写模式
    AsyncFileWriter* asyncWriter_ = nullptr;
    uint64_t currentAsyncId_ = 0;        // 异步写模式下当前分片句柄
    uint64_t sequence_ = 0;
    int64_t segmentStartPts_ = -1;  // 当前分片起始 90kHz PTS
    int64_t lastPts_ = -1;          // 最近写入的 90kHz PTS
    int64_t segmentStartWallUs_ = -1;  // 当前分片首帧的墙钟（UTC 微秒）
    std::atomic<bool> started_{false};
    std::vector<uint64_t> liveSegments_;  // 当前窗口内的分片序列

    std::mutex mu_;
    SegmentCompleteCallback onSegmentComplete_;
};

}  // namespace streaming
