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
#include <vector>

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
    /// 析构：封闭当前分片，并删除磁盘上仍残留的分片文件（live 模式）
    ~Segmenter();

    /// 打开/初始化
    bool open();

    /// 注入异步文件写线程（可选）。设置后所有分片写盘走后台线程，
    /// 不阻塞 Reactor。不设置则回退为同步 std::ofstream。
    void setAsyncWriter(AsyncFileWriter* w) { asyncWriter_ = w; }

    /// 设置 playlist 类型（live/event/vod）。event/vod 不删除旧分片。
    void setPlaylistType(PlaylistType t);

    /// 获取底层 muxer（写入音视频帧）
    TsMuxer& muxer() { return *muxer_; }

    /// 记录最近写入帧的 90kHz 时间戳（用于计算分片时长、首帧定界）
    void noteTimestamp(int64_t ts90k);

    /// 在关键帧处滚动到新分片。
    /// @param boundaryPts 新分片首帧的 90kHz PTS（也即上一分片的结束时刻）。
    void newSegment(int64_t boundaryPts, bool discontinuity = false);

    /// 流结束：封闭当前分片并在其写盘完成后追加 #EXT-X-ENDLIST
    void endOfStream();
    /// 同名流重新推流：清除 ENDLIST，插入 DISCONTINUITY，继续递增分片序号
    void resume();
    bool ended() const;

    /// 兼容旧接口：等同 endOfStream()
    void finalize() { endOfStream(); }

    /// 当前 m3u8 内容（线程安全）
    std::string playlist();

    std::string segmentPath(uint64_t sequence) const;
    uint64_t currentSequence() const { return sequence_; }
    std::string outputDir() const { return outputDir_; }
    std::string streamName() const { return streamName_; }

    void setSegmentCompleteCallback(SegmentCompleteCallback cb);

private:
    // 播放列表状态：可能在写盘线程的关闭回调中修改，故独立出来以 shared_ptr 共享，
    // 回调不依赖 Segmenter 本身的生命周期。
    struct State {
        std::mutex mu;
        M3u8Playlist playlist;
        std::vector<uint64_t> onDisk;  // 已发布、仍在磁盘上的分片序号
        bool cleanup = false;          // Segmenter 已析构：后续完成的分片直接删除
        bool ended = false;
        PlaylistType type = PlaylistType::kLive;
        int keepFiles = 7;             // live 模式磁盘保留数（窗口 + 2）
        SegmentCompleteCallback onComplete;
        State(int target, int window)
            : playlist(target, PlaylistType::kLive, window),
              keepFiles(window + 2) {}
    };

    void writeTs(const uint8_t* data, size_t len);
    /// 关闭当前分片；写盘完成后发布到 m3u8（endList=true 则随后追加 ENDLIST）
    void closeCurrentSegment(int64_t endPts, bool endList);
    static void publishSegment(const std::shared_ptr<State>& st,
                               const std::string& dir, uint64_t seq,
                               double duration, int64_t wallUs, bool valid,
                               bool endList, const std::string& streamName);

    std::string outputDir_;
    std::string streamName_;
    int targetDuration_;
    int windowSize_;

    std::unique_ptr<TsMuxer> muxer_;
    std::shared_ptr<State> st_;

    std::ofstream currentFile_;          // 同步写模式
    AsyncFileWriter* asyncWriter_ = nullptr;
    uint64_t currentAsyncId_ = 0;        // 异步写模式下当前分片句柄
    bool segmentOpen_ = false;
    uint64_t sequence_ = 0;
    int64_t segmentStartPts_ = -1;  // 当前分片起始 90kHz PTS（-1：等首帧）
    int64_t lastPts_ = -1;          // 最近写入的 90kHz PTS
    int64_t segmentStartWallUs_ = -1;  // 当前分片首帧的墙钟（UTC 微秒）
    std::atomic<bool> started_{false};
};

}  // namespace streaming
