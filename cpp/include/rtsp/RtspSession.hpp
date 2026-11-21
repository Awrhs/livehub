#pragma once

#include "core/Buffer.hpp"
#include "net/TcpConnection.hpp"
#include "rtmp/MediaStream.hpp"
#include "rtsp/RtspMessage.hpp"
#include "webrtc/RtpPacket.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

/**
 * RtspSession: 一路 RTSP 播放会话。
 *
 * 支持 RTP over TCP（interleaved 模式，RFC 2326 10.12）：
 *   '$' + channel(1) + len(2) + RTP/RTCP payload
 * 视频走 channel 0（RTP）/1（RTCP），音频走 2/3。
 *
 * 命令流：OPTIONS -> DESCRIBE -> SETUP -> PLAY -> TEARDOWN。
 * PLAY 后订阅 MediaStream，把 H264 打成 RTP 通过 TCP 交织发送。
 */
class RtspSession : public std::enable_shared_from_this<RtspSession> {
public:
    RtspSession(std::shared_ptr<TcpConnection> conn,
                std::shared_ptr<MediaStream> stream);
    ~RtspSession();

    void onRead(Buffer* buf);
    void close();

private:
    void handleRequest(const RtspRequest& req);
    void sendResponse(const RtspResponse& resp);
    void sendInterleaved(uint8_t channel, const uint8_t* data, size_t len);

    void onVideo(const MediaPacket& pkt);
    void onAudio(const MediaPacket& pkt);
    void startPlay();

    std::weak_ptr<TcpConnection> conn_;
    std::weak_ptr<MediaStream> stream_;
    std::string sessionId_;

    bool playing_ = false;
    MediaStream::SubscriberId subId_ = 0;
    std::atomic<bool> alive_{true};

    // RTP 状态
    uint32_t videoSsrc_ = 0x12345678;
    uint32_t audioSsrc_ = 0x87654321;
    uint16_t videoSeq_ = 0;
    uint16_t audioSeq_ = 0;
    uint32_t videoClockBase_ = 0;
    bool haveVideoBase_ = false;

    // 接收侧交织帧重组
    [[maybe_unused]] bool inFrame_ = false;
    [[maybe_unused]] uint8_t frameChannel_ = 0;
    [[maybe_unused]] uint16_t frameLen_ = 0;
    [[maybe_unused]] uint16_t frameGot_ = 0;
    std::vector<uint8_t> frameBuf_;
};

}  // namespace streaming
