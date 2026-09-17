#include "hls/M3u8Playlist.hpp"
#include "test_framework.hpp"

#include <string>

using namespace streaming;

TEST(m3u8_header_fields) {
    M3u8Playlist pl(6, PlaylistType::kLive, 5);
    std::string s = pl.toString();
    ASSERT_TRUE(s.find("#EXTM3U") != std::string::npos);
    ASSERT_TRUE(s.find("#EXT-X-VERSION:3") != std::string::npos);
    ASSERT_TRUE(s.find("#EXT-X-TARGETDURATION:6") != std::string::npos);
}

TEST(m3u8_live_sliding_window) {
    M3u8Playlist pl(6, PlaylistType::kLive, 3);
    for (int i = 0; i < 10; ++i) {
        pl.addSegment(i, 6.0, std::to_string(i) + ".ts");
    }
    std::string s = pl.toString();
    // 窗口只保留 3 个分片：7,8,9.ts
    ASSERT_TRUE(s.find("0.ts") == std::string::npos);
    ASSERT_TRUE(s.find("7.ts") != std::string::npos);
    ASSERT_TRUE(s.find("9.ts") != std::string::npos);
    // media sequence 前移到 7
    ASSERT_TRUE(s.find("#EXT-X-MEDIA-SEQUENCE:7") != std::string::npos);
}

TEST(m3u8_vod_endlist) {
    M3u8Playlist pl(6, PlaylistType::kVod, 100);
    pl.addSegment(0, 6.0, "0.ts");
    pl.addSegment(1, 6.0, "1.ts");
    pl.endList();
    std::string s = pl.toString();
    ASSERT_TRUE(s.find("#EXT-X-PLAYLIST-TYPE:VOD") != std::string::npos);
    ASSERT_TRUE(s.find("#EXT-X-ENDLIST") != std::string::npos);
    ASSERT_TRUE(s.find("0.ts") != std::string::npos);
    ASSERT_TRUE(s.find("1.ts") != std::string::npos);
}

TEST(m3u8_master_playlist) {
    std::vector<std::pair<std::string, int>> variants = {
        {"360p.m3u8", 800000}, {"720p.m3u8", 3000000}};
    std::string s = M3u8Playlist::buildMasterPlaylist(variants);
    ASSERT_TRUE(s.find("#EXT-X-STREAM-INF:BANDWIDTH=800000") !=
                std::string::npos);
    ASSERT_TRUE(s.find("360p.m3u8") != std::string::npos);
    ASSERT_TRUE(s.find("720p.m3u8") != std::string::npos);
}

TEST(m3u8_discontinuity) {
    M3u8Playlist pl(6, PlaylistType::kLive, 10);
    pl.addSegment(0, 6.0, "0.ts");
    pl.setDiscontinuity();
    pl.addSegment(1, 6.0, "1.ts");
    std::string s = pl.toString();
    ASSERT_TRUE(s.find("#EXT-X-DISCONTINUITY") != std::string::npos);
}

TEST(m3u8_event_keeps_all_segments) {
    M3u8Playlist pl(6, PlaylistType::kEvent, 3);
    for (int i = 0; i < 10; ++i) {
        pl.addSegment(i, 6.0, std::to_string(i) + ".ts");
    }
    std::string s = pl.toString();
    // EVENT 模式不删除旧分片（window 不生效），但未 endList
    ASSERT_TRUE(s.find("0.ts") != std::string::npos);
    ASSERT_TRUE(s.find("9.ts") != std::string::npos);
    ASSERT_TRUE(s.find("#EXT-X-PLAYLIST-TYPE:EVENT") != std::string::npos);
    ASSERT_TRUE(s.find("#EXT-X-ENDLIST") == std::string::npos);
}

TEST(m3u8_program_date_time) {
    M3u8Playlist pl(6, PlaylistType::kVod, 10);
    // 2024-01-01T00:00:00.000Z = 1704067200000000 us
    int64_t t = 1704067200LL * 1000000LL;
    pl.addSegment(0, 6.0, "0.ts", t);
    std::string s = pl.toString();
    ASSERT_TRUE(s.find("#EXT-X-PROGRAM-DATE-TIME:2024-01-01T00:00:00.000Z") !=
                std::string::npos);
    // 未提供 programTimeUs 时不输出
    pl.addSegment(1, 6.0, "1.ts");
    s = pl.toString();
    size_t first = s.find("PROGRAM-DATE-TIME");
    size_t second = s.find("PROGRAM-DATE-TIME", first + 1);
    ASSERT_EQ(second, std::string::npos);
}
