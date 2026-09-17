#pragma once

#include <string>
#include <vector>

namespace streaming {

/**
 * Profile: 一条转码输出档的参数（分辨率 / 码率 / 帧率 / GOP）。
 */
struct Profile {
    std::string name;            // e.g. "480p"
    int width = 0;               // 0 表示跟随源
    int height = 0;
    int videoBitrate = 0;        // bps
    int audioBitrate = 0;        // bps
    int fps = 0;                 // 0 表示跟随源
    int gopSize = 0;             // 帧数，0 跟随源 / 2*fps
    std::string videoCodec = "libx264";
    std::string audioCodec = "aac";
    std::string preset = "veryfast";
    std::string tune = "zerolatency";

    /// 预置档位
    static Profile p1080() {
        return {"1080p", 1920, 1080, 4'000'000, 128'000, 30, 60,
                "libx264", "aac", "veryfast", "zerolatency"};
    }
    static Profile p720() {
        return {"720p", 1280, 720, 2'500'000, 128'000, 30, 60,
                "libx264", "aac", "veryfast", "zerolatency"};
    }
    static Profile p480() {
        return {"480p", 854, 480, 1'200'000, 96'000, 30, 60,
                "libx264", "aac", "veryfast", "zerolatency"};
    }
    static Profile p360() {
        return {"360p", 640, 360, 800'000, 64'000, 30, 60,
                "libx264", "aac", "veryfast", "zerolatency"};
    }
};

}  // namespace streaming
