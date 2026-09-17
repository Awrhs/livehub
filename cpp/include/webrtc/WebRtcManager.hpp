#pragma once

#include "net/EventLoop.hpp"
#include "webrtc/DtlsTransport.hpp"
#include "webrtc/WebRtcIngest.hpp"
#include "webrtc/WebRtcTransport.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace streaming {

class MediaStream;

/**
 * WebRtcManager: 管理每路流的 WHEP 观众会话（WebRtcTransport）。
 *
 * 负责分配 UDP 端口、生成 ice-ufrag/ice-pwd/本地 DTLS 指纹，并在
 * WHEP 应答 SDP 中返回 candidate。
 */
class WebRtcManager {
public:
    WebRtcManager(EventLoop* loop, uint16_t portBase = 50000,
                  uint16_t portRange = 1000);
    ~WebRtcManager();

    /// 创建一路下行会话。返回 nullptr 表示端口耗尽。
    std::shared_ptr<WebRtcTransport> createSubscriber(
        const std::string& streamKey,
        std::shared_ptr<MediaStream> stream);

    /// 创建一路上行（WHIP 推流）会话。
    std::shared_ptr<WebRtcIngest> createIngest(
        const std::string& streamKey,
        std::shared_ptr<MediaStream> stream);

    void remove(const std::string& sessionId);

    std::string localFingerprint() const { return localFingerprint_; }

private:
    uint16_t allocPort();

    EventLoop* loop_;
    uint16_t portBase_;
    uint16_t portRange_;
    std::atomic<uint16_t> nextPort_;
    std::string localFingerprint_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<WebRtcTransport>> sessions_;
    std::map<std::string, std::shared_ptr<WebRtcIngest>> ingests_;
};

}  // namespace streaming
