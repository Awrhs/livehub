#pragma once

#include "core/NonCopyable.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace streaming {

class HttpServer;
class MediaStream;

struct StreamInfo {
    std::string key;
    bool hasPublisher = false;
    int64_t viewers = 0;
    int64_t videoBytes = 0;
    int64_t audioBytes = 0;
    int width = 0, height = 0, fps = 0;
    int audioSampleRate = 0, audioChannels = 0;
    int videoBitrate = 0, audioBitrate = 0;
    int64_t uptimeSec = 0;
};

/**
 * StreamsApi: /api/streams 统计与控制接口，以及 ABR 主播放列表。
 *
 * 路由：
 *   GET    /api/streams          列出所有活跃流（观众数、码率、分辨率、时长）
 *   GET    /api/streams/:key     单路流详情
 *   DELETE /api/streams/:key     踢掉一路流
 *   GET    /live/:stream_abr.m3u8  ABR 主播放列表（若 AbrMaster 已设置）
 */
class StreamsApi : NonCopyable {
public:
    using StreamLister = std::function<std::vector<StreamInfo>()>;
    using StreamKiller = std::function<bool(const std::string&)>;
    using AbrMaster = std::function<std::string(const std::string& key)>;

    StreamsApi();

    void setStreamLister(StreamLister l) { list_ = std::move(l); }
    void setStreamKiller(StreamKiller k) { kill_ = std::move(k); }
    void setAbrMaster(AbrMaster m) { abrMaster_ = std::move(m); }

    void attach(HttpServer& server);

private:
    StreamLister list_;
    StreamKiller kill_;
    AbrMaster abrMaster_;
};

}  // namespace streaming
