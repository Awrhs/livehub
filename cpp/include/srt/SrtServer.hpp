#pragma once

#include "core/NonCopyable.hpp"
#include "rtmp/MediaStream.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace streaming {

class EventLoop;

/**
 * SrtServer: 基于 libsrt 的 SRT 接入/分发（MPEG-TS over SRT，live 模式）。
 *
 * streamid（Access Control 语法，兼容 SRS/ffmpeg/OBS）：
 *   "#!::r=live/<key>,m=publish"            推流
 *   "#!::r=live/<key>,m=request"            拉流（m 缺省为 request）
 *   可附带 token：r=live/<key>?token=xxx 或 ",t=xxx"
 *   也接受简写 "live/<key>"、"<key>"
 *
 * 线程模型：独立的 SRT 线程运行 srt_epoll，负责 accept 与推流端读取；
 *   推流数据经 TsDemuxer 还原为 MediaPacket 写入 MediaStream；
 *   拉流端在发布者线程中经 FlvToTs → TsMuxer 封装，非阻塞 srt_sendmsg（发送缓冲满则丢弃）。
 *
 * 未链接 libsrt（HAVE_LIBSRT 未定义）时 start() 仅记录告警，不提供 SRT 服务。
 */
class SrtServer : NonCopyable {
public:
    /// 推流：为 key 返回一个 MediaStream（返回空表示拒绝，如流名已被占用）
    using StreamResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string& key)>;
    /// 拉流：按 key 找已存在的 MediaStream
    using PlayResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string& key)>;
    /// 推流结束
    using UnpublishHandler =
        std::function<void(const std::string& key, const std::shared_ptr<MediaStream>& ms)>;
    /// 鉴权：rawKey 可带 ?token=；返回 false 拒绝
    using AuthHandler = std::function<bool(const std::string& rawKey, bool publish,
                                           std::string* reason)>;

    SrtServer(EventLoop* loop, uint16_t port, const std::string& ip = "0.0.0.0");
    ~SrtServer();

    void start();
    /// 停止 SRT 线程并关闭全部会话（幂等）
    void stop();

    void setStreamResolver(StreamResolver r) { resolver_ = std::move(r); }
    void setPlayResolver(PlayResolver r) { playResolver_ = std::move(r); }
    void setUnpublishHandler(UnpublishHandler h) { onUnpublish_ = std::move(h); }
    void setAuthHandler(AuthHandler h) { auth_ = std::move(h); }

    size_t sessionCount() const;

    /// 解析 streamid → 流名（不含 app 与查询串）、原始 key（含查询串）、是否推流
    static bool parseStreamId(const std::string& sid, std::string* key,
                              std::string* rawKey, bool* publish);

    struct Session;

private:
    void run();
    void acceptOne();
    void closeSession(int sock);

    uint16_t port_;
    std::string ip_;
    StreamResolver resolver_;
    PlayResolver playResolver_;
    UnpublishHandler onUnpublish_;
    AuthHandler auth_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    int listener_ = -1;
    int eid_ = -1;

    mutable std::mutex mu_;
    std::map<int, std::shared_ptr<Session>> sessions_;
};

}  // namespace streaming
