#pragma once

#include "net/EventLoop.hpp"
#include "webrtc/DtlsTransport.hpp"
#include "webrtc/IceMux.hpp"
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
    /// muxPort 非 0 时所有会话共用该 UDP 端口（单端口复用），否则每个会话占用 [portBase, portBase+portRange) 中的一个端口
    WebRtcManager(EventLoop* loop, uint16_t portBase = 50000,
                  uint16_t portRange = 1000, uint16_t muxPort = 0);
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

    /// 清理空闲会话（超过 timeoutMs 未收到对端数据，如浏览器已关闭）；返回清理数
    size_t sweepIdle(int64_t timeoutMs = 30000);
    size_t sessionCount() const;

private:
    uint16_t allocPort();

    EventLoop* loop_;
    std::unique_ptr<IceMux> mux_;
    uint16_t portBase_;
    uint16_t portRange_;
    std::atomic<uint16_t> nextPort_;
    std::string localFingerprint_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<WebRtcTransport>> sessions_;
    std::map<std::string, std::shared_ptr<WebRtcIngest>> ingests_;
};

}  // namespace streaming
