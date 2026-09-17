#pragma once

#include "core/NonCopyable.hpp"
#include "hls/TsDemuxer.hpp"
#include "net/EventLoop.hpp"
#include "rtmp/AacParser.hpp"
#include "rtmp/MediaStream.hpp"
#include "srt/SrtProtocol.hpp"

#include <arpa/inet.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

/**
 * SrtSession: 单条 SRT 连接（一个对端 UDP 地址）。
 *
 * 两种模式：
 *  - ingest（推流）：对端是 caller，发送 TS。我们做三次握手、按序重组、
 *    ACK/NAK，把有序 TS 负载喂给 TsDemuxer，重建 MediaStream。
 *  - play（拉流）：我们把 MediaStream 经 TsMuxer 打包为 TS，
 *    按 seqno 发送并维护发送缓存，响应 NAK 重传、ACK 推进窗口。
 *
 * 线程模型：所有回调在 UdpServer 所属 EventLoop 线程，无需额外加锁，
 * 发送缓存用原子计数 + loop 内访问。
 */
class SrtSession : public NonCopyable,
                   public std::enable_shared_from_this<SrtSession> {
public:
    using SendFn = std::function<void(const std::vector<uint8_t>&,
                                      const sockaddr_in&)>;
    /// ingest 模式：握手拿到 streamid 后，回调返回业务侧 MediaStream，
    /// TsDemuxer 解出的包直接喂给它。
    using IngestResolver =
        std::function<std::shared_ptr<MediaStream>(const std::string& streamId)>;
    /// ingest 结束（对端 shutdown / 会话销毁）时回调，用于清理业务侧流。
    using UnpublishCallback =
        std::function<void(const std::string& streamId)>;

    SrtSession(EventLoop* loop, sockaddr_in peer, uint32_t localSocketId,
               SendFn sender);
    ~SrtSession();

    void setIngestResolver(IngestResolver r) { ingestResolver_ = std::move(r); }
    void setUnpublishCallback(UnpublishCallback cb) { onUnpublish_ = std::move(cb); }

    /// 处理一个到达的 UDP 报文
    void onPacket(const uint8_t* data, size_t len);

    /// 主动以 caller 身份发起连接（向对端推 TS，用于 SRT 转发/推流）
    void connectCaller(const std::string& streamId);

    /// 绑定一个播放流（play 模式）：把该 MediaStream 经 TS 发出
    void attachPlayStream(const std::shared_ptr<MediaStream>& stream);

    void shutdown();

    const sockaddr_in& peer() const { return peer_; }
    bool connected() const { return connected_; }
    bool isPlay() const { return isPlay_; }
    bool playAttached() const { return playAttached_; }
    std::string streamId() const { return streamId_; }

private:
    // ---- 握手 ----
    void handleHandshake(const uint8_t* data, size_t len);
    void sendHandshake(uint32_t type, uint32_t initialSeq,
                       const std::string& streamId);
    void parseSrtExtension(const uint8_t* p, size_t len);

    // ---- 数据接收（ingest）----
    void handleData(const uint8_t* data, size_t len);
    void deliverOrdered();
    void sendAck(uint32_t ackSeq);
    void sendNak(const std::vector<uint32_t>& missing);
    void sendKeepAlive();

    // ---- 数据发送（play/caller push）----
    void onPlayPacket(const MediaPacket& pkt);
    void pumpSend();
    void handleAck(uint32_t ackSeq);
    void handleNak(const uint8_t* data, size_t len);
    void retransmit(uint32_t fromSeq, uint32_t toSeq);

    EventLoop* loop_;
    sockaddr_in peer_;
    SendFn sender_;

    uint32_t localSocketId_;
    uint32_t peerSocketId_ = 0;
    std::string streamId_;
    bool connected_ = false;
    bool isCaller_ = false;
    bool isPlay_ = false;
    bool playAttached_ = false;

    // 接收
    uint32_t rcvNextSeq_ = 1;       // 下一个期望的连续 seq
    std::map<uint32_t, std::vector<uint8_t>> rcvBuffer_;  // 乱序缓存
    TsDemuxer demuxer_;
    IngestResolver ingestResolver_;
    UnpublishCallback onUnpublish_;
    std::shared_ptr<MediaStream> ingestStream_;
    std::string ingestKey_;  // 归一化后的流 key（去掉 #!:: 包装）
    bool ingestPublished_ = false;

    static std::string normalizeResourceKey(const std::string& streamId);

    // 发送
    uint32_t sndNextSeq_ = kDefaultInitialSeq;
    uint32_t sndLastAcked_ = kDefaultInitialSeq - 1;
    struct SentPacket {
        uint32_t seq;
        std::vector<uint8_t> data;
    };
    std::deque<SentPacket> sndBuffer_;  // 已发待确认
    static constexpr size_t kMaxSndBuffer = 4096;
    std::shared_ptr<MediaStream> playStream_;
    MediaStream::SubscriberId playSubId_ = 0;
    AacParser::Config playAacCfg_;
    uint64_t keepaliveTimer_ = 0;
};

}  // namespace streaming
