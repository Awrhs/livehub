#include "rtsp/RtspSession.hpp"
#include "core/Logger.hpp"
#include "rtsp/RtspSdp.hpp"

#include <arpa/inet.h>

#include <cstring>
#include <random>
#include <sstream>

namespace streaming {

namespace {
std::string makeSessionId() {
    static thread_local std::mt19937 rng(std::random_device{}());
    static const char hex[] = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 8; ++i) s.push_back(hex[rng() % 16]);
    return s;
}

std::string base64(const uint8_t* data, size_t len) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) v |= uint32_t(data[i + 2]);
        out.push_back(t[(v >> 18) & 0x3F]);
        out.push_back(t[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? t[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? t[v & 0x3F] : '=');
    }
    return out;
}

// 从 AVCDecoderConfigurationRecord 提取第一个 SPS/PPS
void extractSpsPps(const std::vector<uint8_t>& seq,
                   std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
    if (seq.size() < 8) return;
    size_t off = 5;
    ++off;  // version
    ++off;  // profile
    ++off;  // compat
    ++off;  // level
    if (off >= seq.size()) return;
    ++off;  // naluLenSize
    uint8_t numSps = seq[off++] & 0x1F;
    for (int s = 0; s < numSps && off + 2 <= seq.size(); ++s) {
        uint16_t l = (uint16_t(seq[off]) << 8) | seq[off + 1];
        off += 2;
        if (off + l > seq.size()) return;
        if (s == 0) sps.assign(seq.begin() + off, seq.begin() + off + l);
        off += l;
    }
    if (off >= seq.size()) return;
    uint8_t numPps = seq[off++];
    for (int p = 0; p < numPps && off + 2 <= seq.size(); ++p) {
        uint16_t l = (uint16_t(seq[off]) << 8) | seq[off + 1];
        off += 2;
        if (off + l > seq.size()) return;
        if (p == 0) pps.assign(seq.begin() + off, seq.begin() + off + l);
        off += l;
    }
}

// AVCC → NALU 列表
std::vector<std::vector<uint8_t>> splitAvcc(const uint8_t* d, size_t len) {
    std::vector<std::vector<uint8_t>> out;
    size_t off = 0;
    while (off + 4 <= len) {
        uint32_t l = (uint32_t(d[off]) << 24) | (uint32_t(d[off + 1]) << 16) |
                     (uint32_t(d[off + 2]) << 8) | uint32_t(d[off + 3]);
        off += 4;
        if (off + l > len) break;
        out.emplace_back(d + off, d + off + l);
        off += l;
    }
    return out;
}
}  // namespace

RtspSession::RtspSession(std::shared_ptr<TcpConnection> conn,
                         std::shared_ptr<MediaStream> stream)
    : conn_(std::move(conn)), stream_(std::move(stream)),
      sessionId_(makeSessionId()) {}

RtspSession::~RtspSession() { close(); }

void RtspSession::close() {
    if (!alive_.exchange(false)) return;
    playing_ = false;
    if (auto stream = stream_.lock()) {
        if (subId_) {
            stream->unsubscribe(subId_);
            subId_ = 0;
        }
    }
    if (auto conn = conn_.lock()) {
        conn->shutdown();
    }
}

void RtspSession::onRead(Buffer* buf) {
    // 交织帧与 RTSP 文本可能在同一连接上交错出现，按字节扫描：
    // 若首字节是 '$'，按 interleaved frame 解析；否则按 RTSP 请求解析。
    while (buf->readableBytes() > 0) {
        const char* p = buf->peek();
        if (static_cast<uint8_t>(p[0]) == '$') {
            if (buf->readableBytes() < 4) return;
            uint8_t ch = static_cast<uint8_t>(p[1]);
            uint16_t len = (uint16_t(static_cast<uint8_t>(p[2])) << 8) |
                           static_cast<uint8_t>(p[3]);
            if (buf->readableBytes() < 4u + len) return;
            buf->retrieve(4);
            // 客户端 RTCP（ch=1/3），目前只读取不处理
            buf->retrieve(len);
            (void)ch;
        } else {
            RtspRequest req;
            size_t consumed = parseRtspRequest(p, buf->readableBytes(), req);
            if (consumed == 0) return;
            buf->retrieve(consumed);
            if (!req.method.empty()) handleRequest(req);
        }
    }
}

void RtspSession::sendResponse(const RtspResponse& resp) {
    if (auto conn = conn_.lock()) {
        std::string s = resp.serialize();
        conn->send(s);
    }
}

void RtspSession::sendInterleaved(uint8_t channel, const uint8_t* data,
                                  size_t len) {
    auto conn = conn_.lock();
    if (!conn) return;
    char hdr[4];
    hdr[0] = '$';
    hdr[1] = static_cast<char>(channel);
    hdr[2] = static_cast<char>((len >> 8) & 0xFF);
    hdr[3] = static_cast<char>(len & 0xFF);
    conn->send(std::string(hdr, 4));
    conn->send(std::string(reinterpret_cast<const char*>(data), len));
}

void RtspSession::handleRequest(const RtspRequest& req) {
    RtspResponse resp;
    resp.setHeader("CSeq", std::to_string(req.cseq()));
    if (!sessionId_.empty()) resp.setHeader("Session", sessionId_);

    if (req.method == "OPTIONS") {
        resp.setHeader("Public",
                       "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN");
        sendResponse(resp);
    } else if (req.method == "DESCRIBE") {
        auto stream = stream_.lock();
        std::vector<uint8_t> sps, pps;
        if (stream) extractSpsPps(stream->videoSequenceHeader(), sps, pps);
        std::string sdp =
            buildRtspSdp(req.url, base64(sps.data(), sps.size()),
                         base64(pps.data(), pps.size()));
        resp.setHeader("Content-Type", "application/sdp");
        resp.setHeader("Content-Base", req.url + "/");
        resp.body = sdp;
        sendResponse(resp);
    } else if (req.method == "SETUP") {
        // 解析 Transport: ...;interleaved=0-1
        std::string transport = req.header("Transport");
        std::string interleaved = "0-1";
        auto pos = transport.find("interleaved=");
        if (pos != std::string::npos) {
            interleaved = transport.substr(pos + 12);
            auto comma = interleaved.find(';');
            if (comma != std::string::npos) interleaved = interleaved.substr(0, comma);
        }
        resp.setHeader("Transport",
                       "RTP/AVP/TCP;unicast;interleaved=" + interleaved);
        resp.setHeader("Session", sessionId_ + ";timeout=60");
        sendResponse(resp);
    } else if (req.method == "PLAY") {
        startPlay();
        resp.setHeader("Range", "npt=now-");
        resp.setHeader("RTP-Info", "url=" + req.url + "/trackID=0;seq=0");
        sendResponse(resp);
    } else if (req.method == "TEARDOWN") {
        close();
    } else {
        resp.statusCode = 501;
        resp.reason = "Not Implemented";
        sendResponse(resp);
    }
}

void RtspSession::startPlay() {
    if (playing_) return;
    playing_ = true;
    auto stream = stream_.lock();
    if (!stream) return;
    auto self = shared_from_this();
    subId_ = stream->subscribe([self](const MediaPacket& pkt) {
        if (!self->alive_) return;
        if (pkt.type == MediaPacket::kVideo) self->onVideo(pkt);
        else if (pkt.type == MediaPacket::kAudio) self->onAudio(pkt);
    });
}

void RtspSession::onVideo(const MediaPacket& pkt) {
    if (pkt.data.size() < 5) return;
    uint8_t avcType = pkt.data[1];
    if (avcType == 0) return;  // sequence header，已在 SDP 中携带
    if (avcType != 1) return;

    int32_t cts = (int32_t(uint8_t(pkt.data[2])) << 16) |
                  (int32_t(uint8_t(pkt.data[3])) << 8) |
                  int32_t(uint8_t(pkt.data[4]));
    if (cts & 0x800000) cts |= ~0xFFFFFF;
    auto nalus = splitAvcc(pkt.data.data() + 5, pkt.data.size() - 5);

    uint32_t dts90 = pkt.timestamp * 90;
    uint32_t pts90 = dts90 + uint32_t(cts) * 90;
    if (!haveVideoBase_) {
        videoClockBase_ = pts90;
        haveVideoBase_ = true;
    }
    uint32_t rtpTs = pts90 - videoClockBase_;

    for (size_t i = 0; i < nalus.size(); ++i) {
        const auto& nalu = nalus[i];
        bool isLast = (i + 1 == nalus.size());
        if (nalu.size() <= 1200) {
            RtpHeader h;
            h.marker = isLast ? 1 : 0;
            h.payloadType = 96;
            h.seq = videoSeq_++;
            h.timestamp = rtpTs;
            h.ssrc = videoSsrc_;
            auto pkt = RtpPacket::build(h, nalu.data(), nalu.size());
            sendInterleaved(0, pkt.data(), pkt.size());
        } else {
            // FU-A
            uint8_t nri = (nalu[0] >> 5) & 0x03;
            uint8_t ntype = nalu[0] & 0x1F;
            size_t off = 1, maxChunk = 1200 - 14 - 2;
            bool first = true;
            while (off < nalu.size()) {
                size_t chunk = nalu.size() - off;
                if (chunk > maxChunk) chunk = maxChunk;
                bool last = (off + chunk) >= nalu.size();
                uint8_t payload[1400];
                payload[0] = (nri << 5) | 28;  // FU-A
                payload[1] = ntype;
                if (first) payload[1] |= 0x80;
                if (last) payload[1] |= 0x40;
                std::memcpy(payload + 2, nalu.data() + off, chunk);
                RtpHeader h;
                h.marker = last ? 1 : 0;
                h.payloadType = 96;
                h.seq = videoSeq_++;
                h.timestamp = rtpTs;
                h.ssrc = videoSsrc_;
                auto rtp = RtpPacket::build(h, payload, 2 + chunk);
                sendInterleaved(0, rtp.data(), rtp.size());
                off += chunk;
                first = false;
            }
        }
    }
}

void RtspSession::onAudio(const MediaPacket& pkt) {
    // AAC over RTP 需要 RFC 3640 封装（AU-headers）；这里做最小可用：
    // 4 字节 AU-header-section + 原始 AAC。
    if (pkt.data.size() < 2) return;
    uint8_t pktType = pkt.data[0];
    if ((pktType >> 4) != 0xA) return;  // 非 AAC
    const uint8_t* frame = pkt.data.data() + 1;  // 跳过音频头
    size_t frameLen = pkt.data.size() - 1;
    if (pkt.data[1] == 0x00) {
        // sequence header (ASC)，客户端从 SDP config 获取，忽略
        return;
    }

    uint32_t rtpTs = pkt.timestamp * 44100 / 1000;
    uint8_t payload[1600];
    payload[0] = 0x00;  // AU-headers-length 高字节
    payload[1] = 0x10;  // 16 bits -> 1 AU header
    // AU header: size(13 bits) | index(3 bits)
    uint16_t auh = (static_cast<uint16_t>(frameLen) << 3) | 0;
    payload[2] = auh >> 8;
    payload[3] = auh & 0xFF;
    if (frameLen > sizeof(payload) - 4) frameLen = sizeof(payload) - 4;
    std::memcpy(payload + 4, frame, frameLen);

    RtpHeader h;
    h.marker = 1;
    h.payloadType = 97;
    h.seq = audioSeq_++;
    h.timestamp = rtpTs;
    h.ssrc = audioSsrc_;
    auto rtp = RtpPacket::build(h, payload, 4 + frameLen);
    sendInterleaved(2, rtp.data(), rtp.size());
}

}  // namespace streaming
