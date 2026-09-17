#pragma once

#include "core/NonCopyable.hpp"

#include <functional>
#include <memory>

namespace streaming {

class DvrManager;
class HttpServer;
class MediaStream;

/**
 * DvrHandler: DVR/录制/时移 HTTP 接口。
 *
 * 路由：
 *   POST /api/record/:stream/start    开始录像（VOD fMP4）
 *   POST /api/record/:stream/stop     停止录像
 *   GET  /api/record/:stream/status   录像状态
 *   GET  /timeshift/:stream/init.mp4  时移 init segment
 *   GET  /timeshift/:stream/:seg.m4s  时移媒体分片
 *   GET  /dvr/recordings/:stream.mp4  下载录像文件（VOD，支持 Range）
 */
class DvrHandler : NonCopyable {
public:
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string&)>;

    DvrHandler(DvrManager* dvr, StreamResolver resolver);

    void attach(HttpServer& server);

private:
    DvrManager* dvr_;
    StreamResolver resolver_;
};

}  // namespace streaming
