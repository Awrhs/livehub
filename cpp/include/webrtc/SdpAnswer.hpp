#pragma once

#include <cstdint>
#include <string>

namespace streaming {

/**
 * 按 offer 构造符合 JSEP/RFC 8829 的 answer（WHIP/WHEP 共用）。
 *
 *  - answer 的 m 行与 offer 一一对应、顺序一致；不支持的媒体以端口 0 拒绝且不入 BUNDLE
 *  - 视频选用 offer 中 packetization-mode=1 的 H.264 payload type（保留其 fmtp）
 *  - 复制 offer 的 mid；a=group:BUNDLE 仅包含接受的 mid；会话级 a=ice-lite
 *  - DTLS 角色 passive（等待对端 ClientHello）
 *
 * （原实现无视 offer 结构固定输出单个 m=video/PT 96，标准客户端拒绝该 answer）
 */
struct SdpAnswerParams {
    bool sendVideo = false;          // true: WHEP（服务端发送）；false: WHIP（服务端接收）
    uint16_t port = 9;
    std::string ufrag;
    std::string pwd;
    std::string fingerprint;         // "sha-256 AB:CD:..."
    std::string candidateLine;       // "a=candidate:..."
    uint32_t ssrc = 0;               // sendVideo 时使用
};

struct SdpAnswerResult {
    bool ok = false;                 // offer 中找到可用的 H.264 视频
    std::string sdp;
    int videoPayloadType = -1;
    std::string error;
};

SdpAnswerResult buildSdpAnswer(const std::string& offer, const SdpAnswerParams& p);

/// 提取 offer 中指定算法的 DTLS 指纹（"sha-256 AB:CD:..."）；
/// 对端可能同时给出 sha-256/384/512 多行，必须按算法选取。找不到返回空串。
std::string sdpFingerprint(const std::string& offer, const std::string& algo = "sha-256");

}  // namespace streaming
