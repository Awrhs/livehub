#include "srt/SrtSession.hpp"

#include "core/Logger.hpp"
#include "hls/TsPacket.hpp"
#include "rtmp/AacParser.hpp"
#include "rtmp/EnhancedRtmp.hpp"
#include "rtmp/HevcParser.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>

namespace streaming {

namespace {
constexpr uint32_t kSrtSocketIdListener = 0;
constexpr double kKeepaliveIntervalSec = 1.0;
constexpr double kAckInterval = 0.1;  // 每 100ms ACK 一次
}  // namespace

SrtSession::SrtSession(EventLoop* loop, sockaddr_in peer,
                       uint32_t localSocketId, SendFn sender)
    : loop_(loop),
      peer_(peer),
      sender_(std::move(sender)),
      localSocketId_(localSocketId) {
    demuxer_.setPacketCallback(
        [this](const MediaPacket& pkt) {
            if (ingestStream_) ingestStream_->onPacket(pkt);
        });
}

SrtSession::~SrtSession() {
    if (keepaliveTimer_ && loop_) loop_->cancel(keepaliveTimer_);
    if (isPlay_ && playStream_ && playSubId_) {
        playStream_->unsubscribe(playSubId_);
    }
    if (ingestPublished_ && onUnpublish_) {
        onUnpublish_(ingestKey_);
        ingestPublished_ = false;
    }
}

void SrtSession::onPacket(const uint8_t* data, size_t len) {
    if (len < kSrtHeaderLen) return;
    if (srtIsControl(data)) {
        uint16_t type = srtCtrlType(data);
        const uint8_t* ci = data + kSrtHeaderLen;
        size_t ciLen = len - kSrtHeaderLen;
        switch (static_cast<SrtCtrlType>(type)) {
            case SrtCtrlType::kHandshake:
                handleHandshake(ci, ciLen);
                break;
            case SrtCtrlType::kAck:
                // ACK 序号在包头 Additional Info 字段（bytes 12..15）
                {
                    uint32_t ackSeq =
                        ntohl(*reinterpret_cast<const uint32_t*>(data + 12));
                    handleAck(ackSeq);
                }
                break;
            case SrtCtrlType::kAckAck:
                // ACKACK 的已确认 ACK 序号同样在 bytes 12..15（此处无需动作）
                break;
            case SrtCtrlType::kNak:
                // NAK 丢失列表紧跟在 16 字节包头之后
                handleNak(ci, ciLen);
                break;
            case SrtCtrlType::kKeepAlive:
                // 回应一个 keepalive
                sendKeepAlive();
                break;
            case SrtCtrlType::kShutdown:
                LOG_INFO("SRT peer shutdown streamid=%s", streamId_.c_str());
                connected_ = false;
                if (ingestPublished_ && onUnpublish_) {
                    onUnpublish_(ingestKey_);
                    ingestPublished_ = false;
                }
                if (keepaliveTimer_ && loop_) {
                    loop_->cancel(keepaliveTimer_);
                    keepaliveTimer_ = 0;
                }
                break;
            default:
                break;
        }
        return;
    }
    // 数据包
    handleData(data, len);
}

// ---- 握手（简化版：4 次握手 conclusion/induction/done）----
void SrtSession::handleHandshake(const uint8_t* data, size_t len) {
    if (len < 44) return;
    uint32_t hsType = ntohl(*reinterpret_cast<const uint32_t*>(data));
    peerSocketId_ =
        ntohl(*reinterpret_cast<const uint32_t*>(data + 12));
    // 同步 cookie（data+16..20）
    uint32_t synCookie =
        ntohl(*reinterpret_cast<const uint32_t*>(data + 16));

    // SRT 扩展在握手第 48 字节起
    if (len >= 48 + 8) parseSrtExtension(data + 48, len - 48);

    if (hsType == static_cast<uint32_t>(SrtHandshakeType::kCaller)) {
        isCaller_ = false;  // 对端是 caller，我们是 listener
        // 回应 induction（type=1 但使用对方的 cookie，socket id 指向我们）
        sendHandshake(static_cast<uint32_t>(SrtHandshakeType::kResponder) | 0,
                      kDefaultInitialSeq, streamId_);
        // listener 先发 induction；这里简化：直接回 conclusion
        // 重新发送一个 conclusion(type=kDone)，携带相同 syn cookie
        std::vector<uint8_t> hs(48);
        uint32_t v;
        v = htonl(static_cast<uint32_t>(SrtHandshakeType::kDone));
        std::memcpy(hs.data(), &v, 4);
        v = htonl(kSrtVersion);
        std::memcpy(hs.data() + 4, &v, 4);
        v = htonl(0);  // encryption field (unused)
        std::memcpy(hs.data() + 8, &v, 4);
        v = htonl(localSocketId_);  // our socket id
        std::memcpy(hs.data() + 12, &v, 4);
        v = htonl(synCookie);
        std::memcpy(hs.data() + 16, &v, 4);
        // peer ip (16 bytes, zeros), 其余留 0
        auto ctrl = srtBuildControl(SrtCtrlType::kHandshake, 0, srtNowUs(),
                                    peerSocketId_);
        ctrl.insert(ctrl.end(), hs.begin(), hs.end());
        if (sender_) sender_(ctrl, peer_);
        connected_ = true;
        // streamid 含 "m=publish" 表示推流；否则视为拉流（play 由 server attach）
        if (streamId_.find("m=publish") != std::string::npos) {
            isPlay_ = false;
            startIngest();
        } else {
            isPlay_ = true;
        }
    } else if (hsType == static_cast<uint32_t>(SrtHandshakeType::kDone)) {
        connected_ = true;
        if (isCaller_) {
            startIngest();
        }
    }
}

void SrtSession::sendHandshake(uint32_t type, uint32_t initialSeq,
                               const std::string& streamId) {
    std::vector<uint8_t> hs(128, 0);
    auto put32 = [&](size_t off, uint32_t v) {
        v = htonl(v);
        std::memcpy(hs.data() + off, &v, 4);
    };
    put32(0, type);
    put32(4, kSrtVersion);
    put32(8, 0);                 // encryption
    put32(12, localSocketId_);   // our socket id
    put32(16, 0);                // syn cookie
    put32(44, initialSeq);       // initial packet sequence number

    // SRT extension block: magic 0x4A52, type 1 (handshake), length + SRT HS ext
    size_t ext = 48;
    hs[ext + 0] = 0x4A;
    hs[ext + 1] = 0x52;
    hs[ext + 2] = 0x00;
    hs[ext + 3] = 0x01;  // SRT_CMD_HSREQ/version
    uint16_t blockLen = htons(16);
    std::memcpy(hs.data() + ext + 4, &blockLen, 2);
    // SRT HS extension block (4 bytes version flags)
    put32(ext + 8, 0x00020000);  // SRT version
    put32(ext + 12, 0x00000002); // TSBP mode / TSBPD

    auto ctrl = srtBuildControl(SrtCtrlType::kHandshake, 0, srtNowUs(),
                                peerSocketId_);
    ctrl.insert(ctrl.end(), hs.begin(), hs.end());
    if (sender_) sender_(ctrl, peer_);
}

void SrtSession::parseSrtExtension(const uint8_t* p, size_t len) {
    size_t i = 0;
    while (i + 8 <= len) {
        if (!(p[i] == 0x4A && p[i + 1] == 0x52)) break;
        uint16_t blockSize = ntohs(*reinterpret_cast<const uint16_t*>(p + i + 4));
        uint16_t type = ntohs(*reinterpret_cast<const uint16_t*>(p + i + 2));
        if (type == 0x0002 /*SRT_CMD_SID*/) {
            // stream id block, blockSize bytes after header
            size_t strLen = blockSize > 4 ? blockSize - 4 : 0;
            if (i + 8 + strLen <= len) {
                streamId_.assign(reinterpret_cast<const char*>(p + i + 8),
                                 strLen);
            }
        }
        i += 4 + blockSize;
    }
    if (streamId_.empty()) {
        // 默认用对端地址构造一个 key
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer_.sin_addr, ip, sizeof(ip));
        streamId_ = std::string("srt-") + ip + "-" +
                    std::to_string(ntohs(peer_.sin_port));
    }
}

std::string SrtSession::normalizeResourceKey(const std::string& streamId) {
    // streamid 形如 "#!::r=live/key,m=publish" 或 "live/key"。
    // 提取 r= 后的资源名，并去掉 app/ 前缀，统一为裸 key。
    std::string key = streamId;
    auto rpos = streamId.find("r=");
    if (rpos != std::string::npos) {
        size_t start = rpos + 2;
        size_t end = streamId.find(',', start);
        key = streamId.substr(start, end == std::string::npos
                                          ? std::string::npos : end - start);
    }
    auto slash = key.find('/');
    if (slash != std::string::npos) key = key.substr(slash + 1);
    return key;
}

void SrtSession::startIngest() {
    if (ingestStream_) return;
    ingestKey_ = normalizeResourceKey(streamId_);
    if (ingestResolver_) {
        ingestStream_ = ingestResolver_(ingestKey_);
    }
    if (!ingestStream_) {
        ingestStream_ = std::make_shared<MediaStream>(ingestKey_, "live");
    }
    ingestStream_->setHasPublisher(true);
    ingestPublished_ = true;

    // 启动 keepalive
    std::weak_ptr<SrtSession> w = shared_from_this();
    keepaliveTimer_ = loop_->runEvery(kKeepaliveIntervalSec, [w] {
        if (auto s = w.lock()) s->sendKeepAlive();
    });
}

// ---- 接收 ----
void SrtSession::handleData(const uint8_t* data, size_t len) {
    uint32_t seq = srtDataSeq(data);
    const uint8_t* payload = data + kSrtHeaderLen;
    size_t payloadLen = len - kSrtHeaderLen;

    if (seq < rcvNextSeq_) {
        // 重复包，忽略
        return;
    }
    if (seq == rcvNextSeq_) {
        // 直接交付，推进窗口
        if (payloadLen > 0)
            demuxer_.feed(payload, payloadLen);
        ++rcvNextSeq_;
        deliverOrdered();
    } else {
        // 乱序：缓存
        rcvBuffer_[seq] =
            std::vector<uint8_t>(payload, payload + payloadLen);
    }

    // 周期性 ACK（简化：每收到数据都 ACK 最新连续 seq）
    sendAck(rcvNextSeq_ - 1);

    // 检查缺口并 NAK（seq 比期望大很多时）
    if (seq > rcvNextSeq_) {
        std::vector<uint32_t> missing;
        uint32_t gap = seq - rcvNextSeq_;
        if (gap <= 256) {  // 限制 NAK 范围
            for (uint32_t s = rcvNextSeq_; s < seq; ++s)
                if (rcvBuffer_.find(s) == rcvBuffer_.end()) missing.push_back(s);
        }
        if (!missing.empty()) sendNak(missing);
    }
}

void SrtSession::deliverOrdered() {
    for (;;) {
        auto it = rcvBuffer_.find(rcvNextSeq_);
        if (it == rcvBuffer_.end()) break;
        if (!it->second.empty())
            demuxer_.feed(it->second.data(), it->second.size());
        rcvBuffer_.erase(it);
        ++rcvNextSeq_;
    }
    // 限制乱序缓存，过旧的直接丢弃（防止内存膨胀）
    while (rcvBuffer_.size() > 4096) {
        auto it = rcvBuffer_.begin();
        if (it->first < rcvNextSeq_) rcvBuffer_.erase(it);
        else break;
    }
}

void SrtSession::sendAck(uint32_t ackSeq) {
    // ACK control info field: 4-byte last acknowledged seq
    auto ctrl = srtBuildControl(SrtCtrlType::kAck, ackSeq, srtNowUs(),
                                peerSocketId_);
    if (sender_) sender_(ctrl, peer_);
}

void SrtSession::sendNak(const std::vector<uint32_t>& missing) {
    auto ctrl = srtBuildControl(SrtCtrlType::kNak, 0, srtNowUs(),
                                peerSocketId_);
    for (uint32_t s : missing) {
        uint32_t n = htonl(s | 0x80000000u);  // 单个丢失 seq
        uint8_t b[4];
        std::memcpy(b, &n, 4);
        ctrl.insert(ctrl.end(), b, b + 4);
    }
    if (sender_) sender_(ctrl, peer_);
}

void SrtSession::sendKeepAlive() {
    auto ctrl = srtBuildControl(SrtCtrlType::kKeepAlive, 0, srtNowUs(),
                                peerSocketId_);
    if (sender_) sender_(ctrl, peer_);
}

// ---- 主动 caller（推流到对端）----
void SrtSession::connectCaller(const std::string& streamId) {
    isCaller_ = true;
    streamId_ = streamId;
    peerSocketId_ = 0;
    sendHandshake(static_cast<uint32_t>(SrtHandshakeType::kCaller),
                  kDefaultInitialSeq, streamId);
    std::weak_ptr<SrtSession> w = shared_from_this();
    keepaliveTimer_ = loop_->runEvery(kKeepaliveIntervalSec, [w] {
        if (auto s = w.lock()) s->sendKeepAlive();
    });
}

// ---- play 模式：把 MediaStream 用 TS 发给对端 ----
void SrtSession::attachPlayStream(
    const std::shared_ptr<MediaStream>& stream) {
    isPlay_ = true;
    playStream_ = stream;
    if (!stream) return;
    playAttached_ = true;
    auto muxer = std::make_shared<TsMuxer>(
        [this](const uint8_t* p, size_t n) {
            // 输出 TS 包：按 7*188 字节聚合为 SRT 负载（<=MTU）
            static thread_local std::vector<uint8_t> agg;
            agg.insert(agg.end(), p, p + n);
            if (agg.size() + 188 > kDefaultMtu - kSrtHeaderLen ||
                agg.size() >= 7 * 188) {
                uint32_t seq = sndNextSeq_++;
                auto pkt = srtBuildData(seq, srtNowUs(), localSocketId_,
                                        agg.data(), agg.size());
                if (sender_) sender_(pkt, peer_);
                SentPacket sp;
                sp.seq = seq;
                sp.data = std::move(pkt);
                sndBuffer_.push_back(std::move(sp));
                while (sndBuffer_.size() > kMaxSndBuffer) sndBuffer_.pop_front();
                agg.clear();
            }
        });

    playSubId_ = stream->subscribe(
        [this, muxer](const MediaPacket& pkt) {
            if (!connected_) return;
            // 把 RTMP 包转为 TS 输入（Annex-B 视频 / ADTS 音频）
            if (pkt.type == MediaPacket::kVideo) {
                bool hevc = pkt.videoCodec == VideoCodec::kHevc;
                std::vector<uint8_t> annexb;
                bool isKey = pkt.isKeyframe;
                if (pkt.isSequenceHeader) {
                    if (hevc) {
                        // hvCC -> Annex-B VPS/SPS/PPS
                        if (pkt.enhanced && pkt.data.size() > 6)
                            annexb = HevcParser::buildAnnexBConfig(
                                pkt.data.data() + 6, pkt.data.size() - 6);
                    } else if (pkt.data.size() > 5) {
                        // AVC seq header -> 提取 SPS/PPS 并拼接为 Annex-B
                        std::vector<uint8_t> sps, pps;
                        if (AvcParser::extractSpsPps(
                                pkt.data.data() + 5, pkt.data.size() - 5,
                                sps, pps)) {
                            annexb.insert(annexb.end(), sps.begin(), sps.end());
                            annexb.insert(annexb.end(), pps.begin(), pps.end());
                        }
                    }
                    if (!annexb.empty())
                        muxer->writeVideo(annexb.data(), annexb.size(),
                                          pkt.timestamp * 90LL,
                                          pkt.timestamp * 90LL, true);
                } else {
                    const uint8_t* sample = nullptr;
                    size_t sampleLen = 0;
                    int64_t pts90 = pkt.timestamp * 90LL;
                    int64_t dts90 = pkt.timestamp * 90LL;
                    if (hevc && pkt.enhanced && pkt.data.size() > 9) {
                        sample = pkt.data.data() + 9;
                        sampleLen = pkt.data.size() - 9;
                        int32_t cts = (int32_t(pkt.data[6]) << 16) |
                                      (int32_t(pkt.data[7]) << 8) |
                                      int32_t(pkt.data[8]);
                        if (cts & 0x800000) cts |= 0xFF000000;
                        pts90 = (pkt.timestamp + cts) * 90LL;
                        auto ab = HevcParser::hvccToAnnexB(sample, sampleLen, 3);
                        annexb = std::move(ab);
                        muxer->setVideoStreamType(kStreamTypeHevc);
                    } else if (!hevc && pkt.data.size() > 5) {
                        sample = pkt.data.data() + 5;
                        sampleLen = pkt.data.size() - 5;
                        int32_t cts = (int32_t(pkt.data[2]) << 16) |
                                      (int32_t(pkt.data[3]) << 8) |
                                      int32_t(pkt.data[4]);
                        if (cts & 0x800000) cts |= 0xFF000000;
                        pts90 = (pkt.timestamp + cts) * 90LL;
                        annexb = AvcParser::avccToAnnexB(sample, sampleLen);
                    }
                    if (!annexb.empty())
                        muxer->writeVideo(annexb.data(), annexb.size(), pts90,
                                          dts90, isKey);
                }
            } else if (pkt.type == MediaPacket::kAudio &&
                       pkt.audioCodec == AudioCodec::kAac) {
                if (pkt.isSequenceHeader && pkt.data.size() > 2) {
                    playAacCfg_ = AacParser::parseAudioSpecificConfig(
                        pkt.data.data() + 2, pkt.data.size() - 2);
                } else if (pkt.data.size() > 2 && playAacCfg_.valid) {
                    auto adts = AacParser::wrapWithAdts(
                        playAacCfg_, pkt.data.data() + 2,
                        pkt.data.size() - 2);
                    muxer->writeAudio(adts.data(), adts.size(),
                                      pkt.timestamp * 90LL);
                }
            }
        });
    (void)kAckInterval;
}

void SrtSession::handleAck(uint32_t ackSeq) {
    // 清除已确认的发送缓存
    while (!sndBuffer_.empty() &&
           (int32_t)(sndBuffer_.front().seq - ackSeq) <= 0) {
        sndBuffer_.pop_front();
    }
    sndLastAcked_ = ackSeq;
    // 回 ACKACK
    auto ctrl = srtBuildControl(SrtCtrlType::kAckAck, ackSeq, srtNowUs(),
                                peerSocketId_);
    if (sender_) sender_(ctrl, peer_);
}

void SrtSession::handleNak(const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i + 4 <= len) {
        uint32_t v = ntohl(*reinterpret_cast<const uint32_t*>(data + i));
        if (v & 0x80000000u) {
            // 单个 seq
            uint32_t seq = v & 0x7FFFFFFF;
            retransmit(seq, seq);
            i += 4;
        } else if (i + 8 <= len) {
            uint32_t from = v;
            uint32_t to =
                ntohl(*reinterpret_cast<const uint32_t*>(data + i + 4)) &
                0x7FFFFFFF;
            retransmit(from, to);
            i += 8;
        } else {
            break;
        }
    }
}

void SrtSession::retransmit(uint32_t fromSeq, uint32_t toSeq) {
    for (const auto& sp : sndBuffer_) {
        if (sp.seq >= fromSeq && sp.seq <= toSeq) {
            if (sender_) sender_(sp.data, peer_);
        }
    }
}

void SrtSession::shutdown() {
    auto ctrl = srtBuildControl(SrtCtrlType::kShutdown, 0, srtNowUs(),
                                peerSocketId_);
    if (sender_) sender_(ctrl, peer_);
    connected_ = false;
}

}  // namespace streaming
