#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace streaming {

struct SdpMedia {
    std::string type;        // "video" / "audio"
    int port = 9;
    std::string proto = "UDP/TLS/RTP/SAVPF";
    std::vector<int> payloadTypes;
    std::string direction = "sendonly";  // sendonly / recvonly / sendrecv
    std::string clineIp = "0.0.0.0";
    // ICE
    std::string ufrag;
    std::string pwd;
    // DTLS
    std::string fingerprintHash;  // "sha-256 AB:CD:..."
    std::string dtlsSetup = "actpass";
    // SSRC
    uint32_t ssrc = 0;
    std::string ssrcCname = "streaming";
    // video-specific
    int rtcpMux = 1;
    std::map<int, std::string> rtpmap;   // pt -> "H264/90000"
    std::map<int, std::string> fmtp;     // pt -> "packetization-mode=1;..."
    std::string rtcpFb;                  // goog-remb / nack 等（简化）
};

/**
 * SdpMessage: 简化的 SDP 构建/解析器，仅覆盖 WHIP/WHEP 交换所需字段。
 */
struct SdpMessage {
    std::string userName = "-";
    uint64_t sessionId = 0;
    uint64_t sessionVersion = 0;
    std::string originAddr = "127.0.0.1";
    std::string connectionIp = "0.0.0.0";
    std::vector<SdpMedia> medias;

    std::string serialize() const;
    bool parse(const std::string& text);
};

}  // namespace streaming
