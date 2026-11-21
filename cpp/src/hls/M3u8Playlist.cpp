#include "hls/M3u8Playlist.hpp"

#include <ctime>
#include <cstdio>
#include <sstream>

namespace streaming {

// 把 UTC 微秒时间戳格式化为 EXT-X-PROGRAM-DATE-TIME 所需的 ISO8601 字符串
// 例如 2024-01-02T03:04:05.123Z
static std::string formatProgramDate(int64_t us) {
    time_t sec = static_cast<time_t>(us / 1000000);
    int ms = static_cast<int>((us % 1000000) / 1000);
    struct tm gmt;
    gmtime_r(&sec, &gmt);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &gmt);
    char out[64];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf, ms);
    return out;
}

M3u8Playlist::M3u8Playlist(int targetDuration, PlaylistType type,
                             int windowSize)
    : targetDuration_(targetDuration),
      type_(type),
      windowSize_(windowSize) {}

void M3u8Playlist::addSegment(uint64_t sequence, double duration,
                               const std::string& filename,
                               int64_t programTimeUs) {
    SegmentInfo seg{sequence, duration, filename, discontinuityNext_,
                    programTimeUs};
    discontinuityNext_ = false;
    segments_.push_back(seg);

    // live：滑动窗口淘汰最旧分片
    if (type_ == PlaylistType::kLive &&
        static_cast<int>(segments_.size()) > windowSize_) {
        segments_.pop_front();
    }
}

uint64_t M3u8Playlist::mediaSequence() const {
    if (segments_.empty()) return 0;
    return segments_.front().sequence;
}

std::string M3u8Playlist::toString() const {
    std::ostringstream os;
    os << "#EXTM3U\n";
    os << "#EXT-X-VERSION:3\n";
    if (type_ == PlaylistType::kVod) {
        os << "#EXT-X-PLAYLIST-TYPE:VOD\n";
    } else if (type_ == PlaylistType::kEvent) {
        os << "#EXT-X-PLAYLIST-TYPE:EVENT\n";
    }
    // RFC 8216 4.3.3.1：TARGETDURATION 必须 >= 每个分片 EXTINF 四舍五入后的整数
    int target = targetDuration_;
    for (const auto& seg : segments_) {
        int d = static_cast<int>(seg.duration + 0.5);
        if (d > target) target = d;
    }
    os << "#EXT-X-TARGETDURATION:" << target << "\n";
    os << "#EXT-X-MEDIA-SEQUENCE:" << mediaSequence() << "\n";

    for (const auto& seg : segments_) {
        if (seg.isDiscontinuity) {
            os << "#EXT-X-DISCONTINUITY\n";
        }
        if (seg.programTimeUs >= 0) {
            os << "#EXT-X-PROGRAM-DATE-TIME:"
               << formatProgramDate(seg.programTimeUs) << "\n";
        }
        char dur[16];
        std::snprintf(dur, sizeof(dur), "%.3f", seg.duration);
        os << "#EXTINF:" << dur << ",\n";
        os << seg.filename << "\n";
    }

    if (ended_) {
        os << "#EXT-X-ENDLIST\n";
    }
    return os.str();
}

std::string M3u8Playlist::buildMasterPlaylist(
    const std::vector<std::pair<std::string, int>>& variants) {
    std::ostringstream os;
    os << "#EXTM3U\n";
    os << "#EXT-X-VERSION:3\n";
    for (const auto& v : variants) {
        os << "#EXT-X-STREAM-INF:BANDWIDTH=" << v.second << "\n";
        os << v.first << "\n";
    }
    return os.str();
}

}  // namespace streaming
