/**
 * whip_client: 通过 WHIP（WebRTC-HTTP Ingestion Protocol）把一个 FLV/H264
 * 文件推流到服务器（命令行工具样例）。
 *
 * 用法:
 *   whip_client <flv-file> <whip-url>
 *   例: whip_client test.flv http://127.0.0.1:8080/whip/test
 *
 * 流程：
 *   1. 构造 SDP offer（video sendonly，H264/90000 PT96，本地 ICE 候选）。
 *   2. POST offer 到 WHIP 端点，解析 answer 的 ufrag/pwd/fingerprint/candidate。
 *   3. 绑定本地 UDP，向对端候选发送 STUN Binding Request（ICE 全量角色）。
 *   4. 收到 STUN 响应后锁定候选对，进行 DTLS 客户端握手，导出 SRTP key。
 *   5. 解析 FLV（FlvDemuxer）提取 H264 NALU，用 H264RtpPacketizer 打包，
 *      SRTP 保护后通过 UDP 发送，按 timestamp 实时节奏。
 *
 * 简历点：WebRTC 客户端、WHIP、ICE/DTLS-SRTP、RTP 打包、NACK/PLI 反馈解析。
 */
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "net/EventLoop.hpp"
#include "net/UdpServer.hpp"
#include "protocol/FlvDemuxer.hpp"
#include "rtmp/AvcParser.hpp"
#include "webrtc/DtlsTransport.hpp"
#include "webrtc/RtpPacket.hpp"
#include "webrtc/Rtcp.hpp"
#include "webrtc/Sdp.hpp"
#include "webrtc/SrtpSession.hpp"
#include "webrtc/Stun.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace streaming;

namespace {

// ---- HTTP URL ----
struct HttpUrl {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};
bool parseHttp(const std::string& url, HttpUrl& out) {
    auto pos = url.find("://");
    std::string rest = (pos == std::string::npos) ? url : url.substr(pos + 3);
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    auto colon = hostport.find(':');
    if (colon == std::string::npos) {
        out.host = hostport;
        out.port = 80;
    } else {
        out.host = hostport.substr(0, colon);
        out.port = static_cast<uint16_t>(std::stoi(hostport.substr(colon + 1)));
    }
    return true;
}

// ---- 阻塞式 HTTP POST（返回 body） ----
std::string httpPost(const HttpUrl& url, const std::string& body,
                     const std::string& contentType) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints,
                    &res) != 0)
        return "";
    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return "";

    std::string req = "POST " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + url.host + "\r\n";
    req += "Content-Type: " + contentType + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    if (::send(fd, req.data(), req.size(), 0) <= 0) {
        ::close(fd);
        return "";
    }
    std::string resp;
    char tmp[4096];
    ssize_t n;
    while ((n = ::recv(fd, tmp, sizeof(tmp), 0)) > 0) resp.append(tmp, n);
    ::close(fd);
    auto bodyPos = resp.find("\r\n\r\n");
    if (bodyPos == std::string::npos) return "";
    return resp.substr(bodyPos + 4);
}

// 在 SDP answer 中抽取候选 ip/port
bool parseCandidate(const std::string& sdp, sockaddr_in& peer) {
    auto pos = sdp.find("a=candidate:");
    if (pos == std::string::npos) return false;
    std::string line = sdp.substr(pos);
    auto eol = line.find("\r\n");
    if (eol != std::string::npos) line = line.substr(0, eol);
    // candidate:foundation component proto priority ip port typ host ...
    int comp = 0;
    std::string ip;
    int port = 0;
    int field = 0;
    size_t i = 12;  // skip "a=candidate:"
    while (i < line.size()) {
        auto sp = line.find(' ', i);
        std::string tok = line.substr(i, sp == std::string::npos ? std::string::npos
                                                                 : sp - i);
        ++field;
        if (field == 2) comp = std::stoi(tok);
        else if (field == 5) ip = tok;
        else if (field == 6) port = std::stoi(tok);
        if (sp == std::string::npos) break;
        i = sp + 1;
        (void)comp;
    }
    if (ip.empty() || port == 0) return false;
    std::memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<uint16_t>(port));
    return ::inet_pton(AF_INET, ip.c_str(), &peer.sin_addr) == 1;
}

std::string sdpAttr(const std::string& sdp, const std::string& key) {
    std::string needle = "a=" + key;
    auto p = sdp.find(needle);
    if (p == std::string::npos) return "";
    auto colon = sdp.find(':', p);
    auto eol = sdp.find("\r\n", p);
    if (colon == std::string::npos || colon > eol) return "";
    return sdp.substr(colon + 1, eol - colon - 1);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <flv-file> <whip-url>\n", argv[0]);
        return 1;
    }
    std::string flvPath = argv[1];
    std::string whipUrl = argv[2];

    EventLoop loop;
    // 让 UDP/STUN/DTLS 在后台 EventLoop 线程处理
    std::thread loopThread([&] { loop.loop(); });

    // ---- 本地 UDP ----
    UdpServer udp(&loop, 0, "0.0.0.0");
    udp.start();
    uint16_t localPort = udp.port();

    std::string localUfrag = "ufrag" + std::to_string(localPort);
    std::string localPwd = "localpassword" + std::to_string(localPort);
    std::string localFp = DtlsTransport::localFingerprintSha256();

    // ---- 构造 offer ----
    std::ostringstream offer;
    offer << "v=0\r\n";
    offer << "o=- 0 0 IN IP4 127.0.0.1\r\n";
    offer << "s=streaming-cpp whip-client\r\n";
    offer << "t=0 0\r\n";
    offer << "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n";
    offer << "c=IN IP4 0.0.0.0\r\n";
    offer << "a=ice-ufrag:" << localUfrag << "\r\n";
    offer << "a=ice-pwd:" << localPwd << "\r\n";
    offer << "a=ice-options:trickle\r\n";
    offer << "a=fingerprint:" << localFp << "\r\n";
    offer << "a=setup:actpass\r\n";
    offer << "a=sendonly\r\n";
    offer << "a=rtcp-mux\r\n";
    offer << "a=rtpmap:96 H264/90000\r\n";
    offer << "a=fmtp:96 packetization-mode=1\r\n";
    offer << "a=candidate:1 1 udp 2130706431 127.0.0.1 " << localPort
          << " typ host\r\n";
    offer << "a=end-of-candidates\r\n";

    // ---- POST offer ----
    HttpUrl url;
    parseHttp(whipUrl, url);
    std::string answer = httpPost(url, offer.str(), "application/sdp");
    if (answer.empty()) {
        std::fprintf(stderr, "WHIP POST failed\n");
        return 1;
    }
    std::string remoteUfrag = sdpAttr(answer, "ice-ufrag");
    std::string remotePwd = sdpAttr(answer, "ice-pwd");
    std::string remoteFp = sdpAttr(answer, "fingerprint");
    sockaddr_in peer{};
    if (!parseCandidate(answer, peer)) {
        std::fprintf(stderr, "no candidate in answer\n");
        return 1;
    }
    std::fprintf(stdout, "WHIP answer: ufrag=%s peer=%s:%u\n",
                 remoteUfrag.c_str(), inet_ntoa(peer.sin_addr),
                 ntohs(peer.sin_port));

    // ---- DTLS（客户端）----
    DtlsTransport dtls(DtlsTransport::Role::kClient, localFp);
    dtls.setRemoteFingerprint(remoteFp);
    SrtpSession srtp;
    std::atomic<bool> ready{false};
    std::vector<uint8_t> srtpKey, srtpSalt;

    // 发往对端的统一下行口（udp.sendto）
    auto sendUdp = [&](const uint8_t* d, size_t n) {
        udp.sendto(d, n, peer);
    };
    dtls.setSendCallback(sendUdp);
    dtls.setReadyCallback([&](const std::vector<uint8_t>& k,
                              const std::vector<uint8_t>& s) {
        if (!k.empty()) {
            srtp.initSend(k, s);
            srtpKey = k;
            srtpSalt = s;
        }
        ready = true;
        LOG_INFO("WHIP client: DTLS ready, SRTP %s",
                 k.empty() ? "passthrough" : "enabled");
    });
    dtls.init();

    // ---- ICE 状态 ----
    std::atomic<bool> selected{false};
    auto doStunCheck = [&]() {
        // USERNAME = remoteUfrag:localUfrag
        std::string uname = remoteUfrag + ":" + localUfrag;
        // password 为远端 ice-pwd
        auto req = Stun::buildBindingRequest(uname, 1862270975u,
                                             0x1234567812345678ULL, true,
                                             remotePwd);
        udp.sendto(req.data(), req.size(), peer);
    };

    // RTP 打包器
    uint32_t ssrc = 0xCAFEBABE;
    H264RtpPacketizer packetizer(ssrc, 1200);
    packetizer.setPayloadType(96);
    packetizer.setCallback([&](const uint8_t* d, size_t n) {
        uint8_t buf[2048];
        if (n > sizeof(buf)) return;
        std::memcpy(buf, d, n);
        int outLen = srtp.protectRtp(buf, static_cast<int>(n), sizeof(buf));
        if (outLen > 0) udp.sendto(buf, static_cast<size_t>(outLen), peer);
    });

    udp.setMessageCallback(
        [&](const uint8_t* d, size_t n, const sockaddr_in&, Timestamp) {
            bool isStun = n >= 20 && (d[0] & 0xC0) == 0 &&
                          d[4] == 0x21 && d[5] == 0x12 && d[6] == 0xA4 &&
                          d[7] == 0x42;
            if (isStun) {
                StunMessage msg;
                if (Stun::parse(d, n, msg) &&
                    msg.type == kStunBindingResponse) {
                    selected = true;
                    LOG_INFO("ICE selected");
                    // 启动 DTLS 握手
                    if (!dtls.established()) dtls.startHandshake();
                }
                return;
            }
            // 非 STUN：DTLS / SRTP
            if (!dtls.established()) {
                dtls.feed(d, n);
                return;
            }
            // 解密 RTCP（PLI/NACK）—— 客户端发送侧一般忽略
            uint8_t tmp[2048];
            if (n > sizeof(tmp)) return;
            std::memcpy(tmp, d, n);
            int dec = srtp.unprotectRtcp(tmp, static_cast<int>(n));
            if (dec > 0) {
                Rtcp::Feedback fb;
                Rtcp::parse(tmp, static_cast<size_t>(dec), fb);
                if (fb.isPli) LOG_INFO("received PLI (keyframe request)");
            }
        });

    // ---- 发 STUN 探测（定时直到选中）----
    doStunCheck();
    auto stunTimer = loop.runEvery(0.2, [&] {
        if (selected) return;
        doStunCheck();
    });
    int stunTries = 0;
    while (!selected && stunTries++ < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!selected) {
        std::fprintf(stderr, "ICE failed\n");
        loop.quit();
        loopThread.join();
        return 1;
    }
    // 等待 DTLS 握手完成
    int dtlsTries = 0;
    while (!ready && dtlsTries++ < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    (void)stunTimer;

    // ---- 读取 FLV，提取 H264 NALU 并发送 ----
    FILE* flv = std::fopen(flvPath.c_str(), "rb");
    if (!flv) {
        std::fprintf(stderr, "open flv failed: %s\n", flvPath.c_str());
        return 1;
    }
    std::fseek(flv, 0, SEEK_END);
    long fsz = std::ftell(flv);
    std::fseek(flv, 0, SEEK_SET);
    std::vector<uint8_t> flvData(static_cast<size_t>(fsz));
    std::fread(flvData.data(), 1, flvData.size(), flv);
    std::fclose(flv);

    // sequence header 中的 SPS/PPS
    std::vector<uint8_t> sps, pps;
    auto start = std::chrono::steady_clock::now();
    uint32_t firstTs = UINT32_MAX;
    FlvDemuxer demux;
    demux.setTagCallback([&](const MediaPacket& pkt) {
        if (pkt.type != MediaPacket::kVideo) return;
        const uint8_t* d = pkt.data.data();
        size_t n = pkt.data.size();
        if (n < 2) return;
        int avcType = d[1];
        if (avcType == 0) {
            AvcParser::extractSpsPps(d + 5, n - 5, sps, pps);
            // extractSpsPps 带 start code，去掉前 4 字节给 packetizer
            if (sps.size() > 4) sps.erase(sps.begin(), sps.begin() + 4);
            if (pps.size() > 4) pps.erase(pps.begin(), pps.begin() + 4);
            return;
        }
        if (avcType != 1) return;
        if (firstTs == UINT32_MAX) firstTs = pkt.timestamp;
        uint32_t rel = pkt.timestamp - firstTs;

        // 实时节奏
        std::this_thread::sleep_until(
            start + std::chrono::milliseconds(rel));

        // AVCC -> NALUs (去掉 4 字节长度前缀)
        std::vector<std::vector<uint8_t>> nalus;
        if (!sps.empty()) {
            nalus.push_back(sps);
            nalus.push_back(pps);
        }
        const uint8_t* p = d + 5;
        size_t left = n - 5;
        while (left >= 4) {
            uint32_t ln = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                          (uint32_t(p[2]) << 8) | p[3];
            p += 4; left -= 4;
            if (ln > left) break;
            nalus.emplace_back(p, p + ln);
            p += ln; left -= ln;
        }
        uint32_t ts90 = rel * 90u;
        packetizer.packetize(nalus, ts90,
                             AvcParser::isKeyframe(d, n));
        // 只在每个 IDR 前带一次 SPS/PPS，发完清空
        if (!sps.empty()) { sps.clear(); pps.clear(); }
    });
    demux.feed(flvData.data(), flvData.size());

    std::fprintf(stdout, "WHIP push complete\n");
    loop.quit();
    loopThread.join();
    return 0;
}
