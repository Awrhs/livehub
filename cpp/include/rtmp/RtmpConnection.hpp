#pragma once

#include "core/NonCopyable.hpp"
#include "net/TcpConnection.hpp"
#include "rtmp/Amf0.hpp"
#include "rtmp/Chunk.hpp"
#include "rtmp/Handshake.hpp"

#include <functional>
#include <memory>
#include <string>

namespace streaming {

class MediaStream;

/**
 * RtmpConnection: 一条 RTMP 连接的状态机。
 *
 * 流程：握手 → 收 chunk → 重组消息 → 处理 AMF0 命令 → 收发音视频。
 *
 * 处理的命令：connect / releaseStream / FCPublish / createStream /
 *             publish / play / deleteStream / FCUnpublish。
 * 推流时把音视频消息交给 MediaStream；播放时订阅 MediaStream 推送。
 */
class RtmpConnection : NonCopyable,
                        public std::enable_shared_from_this<RtmpConnection> {
public:
    using PublishCallback =
        std::function<void(const std::shared_ptr<RtmpConnection>&,
                           const std::string& app, const std::string& streamKey,
                           std::shared_ptr<MediaStream>)>;
    using PlayCallback =
        std::function<void(const std::shared_ptr<RtmpConnection>&,
                           const std::string& app, const std::string& streamKey)>;
    using CloseCallback =
        std::function<void(const std::shared_ptr<RtmpConnection>&)>;
    /// 鉴权回调：publish=true 为推流，false 为拉流。
    /// 返回 true 放行；返回 false 时 *reason 填入拒绝原因（可空）。
    using AuthCallback = std::function<bool(
        const std::string& host, const std::string& app,
        const std::string& streamKey, bool publish, std::string* reason)>;

    RtmpConnection(TcpConnectionPtr conn, std::string app = "live");
    ~RtmpConnection();

    void setPublishCallback(PublishCallback cb) { onPublish_ = std::move(cb); }
    void setPlayCallback(PlayCallback cb) { onPlay_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { onClose_ = std::move(cb); }
    void setAuthCallback(AuthCallback cb) { onAuth_ = std::move(cb); }

    /// 由 TcpServer 消息回调驱动
    void onRead(Buffer* buf);

    void sendMessage(const RtmpMessage& msg);

    bool isPublisher() const { return publishing_; }
    bool isPlayer() const { return playing_; }
    const std::string& app() const { return app_; }
    const std::string& streamKey() const { return streamKey_; }
    /// 从 tcUrl 解析出的主机名（vhost 匹配用）；未知时为空。
    const std::string& host() const { return host_; }
    TcpConnectionPtr tcpConn() const { return conn_.lock(); }
    std::shared_ptr<MediaStream> stream() const { return stream_; }

    void close();

private:
    void onHandshakeDone();
    void onCompleteMessage(RtmpMessage&& msg);
    void handleCommand(const RtmpMessage& msg);
    void handleData(const RtmpMessage& msg);
    void handleAudio(const RtmpMessage& msg);
    void handleVideo(const RtmpMessage& msg);

    // 命令应答
    void respondConnect(double transactionId, const std::string& app);
    void respondCreateStream(double transactionId);
    void sendWindowAckSize(uint32_t size);
    void sendSetPeerBandwidth(uint32_t size);
    void sendSetChunkSize(uint32_t size);
    void sendStreamBegin(uint32_t streamId);
    void onStatus(uint32_t streamId, const std::string& level,
                  const std::string& code, const std::string& description);

    std::weak_ptr<TcpConnection> conn_;
    std::string app_;
    std::string tcUrl_;
    std::string host_;
    std::string streamKey_;

    Handshake handshake_;
    ChunkParser chunkParser_;
    ChunkWriter chunkWriter_;

    bool handshakeDone_ = false;
    bool publishing_ = false;
    bool playing_ = false;
    uint32_t clientChunkSize_ = 128;
    double nextTransactionId_ = 1.0;

    std::shared_ptr<MediaStream> stream_;
    uint64_t subscriberId_ = 0;

    PublishCallback onPublish_;
    PlayCallback onPlay_;
    CloseCallback onClose_;
    AuthCallback onAuth_;
};

}  // namespace streaming
