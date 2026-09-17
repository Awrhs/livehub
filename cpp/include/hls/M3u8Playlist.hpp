#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace streaming {

enum class PlaylistType { kLive, kEvent, kVod };

struct SegmentInfo {
    uint64_t sequence;
    double duration;       // 秒
    std::string filename;  // 例如 12.ts
    bool isDiscontinuity = false;
    int64_t programTimeUs = -1;  // EXT-X-PROGRAM-DATE-TIME（微秒，UTC）；-1 不输出
};

/**
 * M3u8Playlist: HLS m3u8 播放列表生成。
 *
 * 维护滑动窗口（live）或不断增长（event/vod）的分片列表，
 * 输出符合 HLS 规范的 #EXTM3U 文本。
 */
class M3u8Playlist {
public:
    explicit M3u8Playlist(int targetDuration = 6,
                           PlaylistType type = PlaylistType::kLive,
                           int windowSize = 5);

    void setTargetDuration(int d) { targetDuration_ = d; }
    void setWindowSize(int n) { windowSize_ = n; }
    void setType(PlaylistType t) { type_ = t; }
    void setDiscontinuity() { discontinuityNext_ = true; }

    /// 添加一个分片
    void addSegment(uint64_t sequence, double duration,
                    const std::string& filename,
                    int64_t programTimeUs = -1);

    /// 触发 VOD/EVENT 结束
    void endList() { ended_ = true; }

    uint64_t mediaSequence() const;
    std::string toString() const;

    /// 生成主 playlist（多码率）
    static std::string buildMasterPlaylist(
        const std::vector<std::pair<std::string, int>>& variants);

private:
    int targetDuration_;
    PlaylistType type_;
    int windowSize_;
    bool ended_ = false;
    bool discontinuityNext_ = false;
    std::deque<SegmentInfo> segments_;
};

}  // namespace streaming
