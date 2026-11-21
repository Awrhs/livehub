#include "rtmp/RtmpConnection.hpp"
#include "rtmp/FlowControl.hpp"

#include "core/Buffer.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "net/TcpConnection.hpp"
#include "rtmp/MediaStream.hpp"
#include "rtmp/AacParser.hpp"
#include "rtmp/AvcParser.hpp"
#include "rtmp/EnhancedRtmp.hpp"
#include "rtmp/HevcParser.hpp"

#include <cstring>

namespace streaming {

namespace {
constexpr uint32_t kServerChunkSize = 4096;
constexpr uint32_t kWindowAckSize = 5000000;
constexpr uint32_t kPeerBandwidth = 5000000;
constexpr uint32_t kDefaultStreamId = 1;

// 从 tcUrl（形如 rtmp://host[:port]/app[/key]）中解析主机名，用于 vhost 匹配。
std::string parseHostFromTcUrl(const std::string& tcUrl) {
    const std::string sep = "://";
    auto p = tcUrl.find(sep);
    size_t start = (p == std::string::npos) ? 0 : p + sep.size();
    if (start >= tcUrl.size()) return "";
    size_t end = tcUrl.find('/', start);
    std::string hostPort = tcUrl.substr(start, end - start);
    auto colon = hostPort.rfind(':');
    if (colon != std::string::npos) hostPort = hostPort.substr(0, colon);
    return hostPort;
}
}  // namespace

RtmpConnection::RtmpConnection(TcpConnectionPtr conn, std::string app)
    : conn_(conn), app_(std::move(app)) {
    chunkWriter_.setChunkSize(kServerChunkSize);
}

RtmpConnection::~RtmpConnection() {
    if (stream_ && subscriberId_ != 0) {
        stream_->unsubscribe(subscriberId_);
    }
}

void RtmpConnection::onRead(Buffer* buf) {
    if (!handshakeDone_) {
        Buffer out;
        bool done = handshake_.onRead(*buf, out);
        auto c = conn_.lock();
        if (c && out.readableBytes() > 0) c->send(&out);
        if (handshake_.failed()) {
            buf->retrieveAll();
            if (c) c->forceClose();
            return;
        }
        // 注意：收到 C0C1 只是发出 S0S1S2，必须等 C2 到达才算握手完成；
        // 原实现在 C0C1 后即进入 chunk 解析，把 C2 当作 chunk 导致所有连接失败。
        if (!done) return;  // 半包或等待 C2
        onHandshakeDone();
    }

    // 握手完成后持续解析 chunk
    RtmpMessage msg;
    while (!closed_ && chunkParser_.parseMessage(*buf, msg)) {
        onCompleteMessage(std::move(msg));
        msg = RtmpMessage{};
    }
    if (closed_) {
        buf->retrieveAll();
        return;
    }
    if (chunkParser_.hasError()) {
        // 协议错误：后续字节流已无法对齐，直接断开
        LOG_WARN("rtmp: chunk protocol error, closing connection");
        buf->retrieveAll();
        if (auto c = conn_.lock()) c->forceClose();
    }
}

void RtmpConnection::onHandshakeDone() {
    handshakeDone_ = true;
    // 主动告知对端本端 chunk size 与窗口
    sendSetChunkSize(kServerChunkSize);
    sendWindowAckSize(kWindowAckSize);
    sendSetPeerBandwidth(kPeerBandwidth);
}

void RtmpConnection::onCompleteMessage(RtmpMessage&& msg) {
    switch (msg.typeId) {
        case kMsgSetChunkSize:
            if (msg.payload.size() >= 4) {
                uint32_t sz = (static_cast<uint32_t>(msg.payload[0]) << 24) |
                              (static_cast<uint32_t>(msg.payload[1]) << 16) |
                              (static_cast<uint32_t>(msg.payload[2]) << 8) |
                              static_cast<uint32_t>(msg.payload[3]);
                sz &= 0x7FFFFFFF;
                if (sz >= 128) {
                    clientChunkSize_ = sz;
                    chunkParser_.setChunkSize(sz);
                }
            }
            break;
        case kMsgWindowAckSize:
        case kMsgSetPeerBandwidth:
        case kMsgAcknowledgement:
        case kMsgUserControlMessage:
            // 客户端控制消息，本端无需响应
            break;
        case kMsgCommandAmf0:
            handleCommand(msg);
            break;
        case kMsgDataAmf0:
            handleData(msg);
            break;
        case kMsgAudio:
            handleAudio(msg);
            break;
        case kMsgVideo:
            handleVideo(msg);
            break;
        default:
            // 忽略未知类型
            break;
    }
}

void RtmpConnection::handleCommand(const RtmpMessage& msg) {
    Buffer buf;
    buf.append(reinterpret_cast<const char*>(msg.payload.data()),
               msg.payload.size());

    Amf0Value nameVal;
    Amf0Value txVal;
    if (!nameVal.decode(buf) || !txVal.decode(buf)) return;
    std::string cmd = nameVal.asString();
    double txId = txVal.asNumber(0.0);

    // command object（第 3 个参数）
    Amf0Value cmdObj;
    cmdObj.decode(buf);

    LOG_DEBUG("RTMP command: %s tx=%g app=%s", cmd.c_str(), txId,
              app_.c_str());

    if (cmd == "connect") {
        if (const auto* obj = cmdObj.asObject()) {
            auto it = obj->find("app");
            if (it != obj->end()) app_ = it->second.asString(app_);
            auto tu = obj->find("tcUrl");
            if (tu != obj->end()) {
                tcUrl_ = tu->second.asString();
                host_ = parseHostFromTcUrl(tcUrl_);
            }
        }
        respondConnect(txId, app_);
    } else if (cmd == "releaseStream" || cmd == "FCPublish" ||
               cmd == "FCUnpublish") {
        // 这些命令回 _result 即可
        Amf0Value null = Amf0Value::null();
        Amf0Value result = Amf0Value::object(Amf0Value::Object{});
        RtmpMessage resp;
        resp.typeId = kMsgCommandAmf0;
        resp.chunkStreamId = kCsIdInvoke;
        resp.streamId = 0;
        Buffer pb;
        Amf0Value::str("_result").encode(pb);
        Amf0Value::number(txId).encode(pb);
        null.encode(pb);
        result.encode(pb);
        resp.payload.assign(pb.peek(), pb.peek() + pb.readableBytes());
        sendMessage(resp);
    } else if (cmd == "createStream") {
        respondCreateStream(txId);
    } else if (cmd == "publish") {
        // 第 4 参数为 stream key（可能在 cmdObj 之后）
        std::string rawKey, query;
        Amf0Value keyVal;
        if (keyVal.decode(buf)) rawKey = keyVal.asString();
        splitStreamKey(rawKey, &streamKey_, &query);
        if (streamKey_.empty()) {
            onStatus(kDefaultStreamId, "error", "NetStream.Publish.BadName",
                     "empty stream name");
            close();
            return;
        }

        // 鉴权：按 vhost 配置进行 webhook/JWT 校验（传入原始 key，含 ?token=）
        if (onAuth_) {
            std::string reason;
            if (!onAuth_(host_, app_, rawKey, true, &reason)) {
                LOG_WARN("RTMP publish denied: app=%s key=%s host=%s reason=%s",
                         app_.c_str(), streamKey_.c_str(), host_.c_str(),
                         reason.c_str());
                onStatus(kDefaultStreamId, "error",
                         "NetStream.Publish.BadName",
                         reason.empty() ? "publish denied" : reason);
                close();
                return;
            }
        }

        publishing_ = true;
        // 创建/绑定 MediaStream 由上层完成
        if (onPublish_) {
            auto ms = std::make_shared<MediaStream>(streamKey_, app_);
            ms->setHasPublisher(true);
            ms->setVhost(host_);
            ms->touch(Timestamp::now().microSecondsSinceEpoch() / 1000);
            stream_ = ms;
            if (!onPublish_(shared_from_this(), app_, streamKey_, ms)) {
                // 同名流正在推送：拒绝，避免覆盖（流劫持）
                LOG_WARN("RTMP publish rejected, stream busy: %s",
                         streamKey_.c_str());
                onStatus(kDefaultStreamId, "error", "NetStream.Publish.BadName",
                         "stream already publishing: " + streamKey_);
                close();
                return;
            }
        }
        onStatus(kDefaultStreamId, "status", "NetStream.Publish.Start",
                 "Publishing " + streamKey_);
    } else if (cmd == "play") {
        std::string rawKey, query;
        Amf0Value keyVal;
        if (keyVal.decode(buf)) rawKey = keyVal.asString();
        splitStreamKey(rawKey, &streamKey_, &query);

        // 鉴权（传入原始 key，含 ?token=）
        if (onAuth_) {
            std::string reason;
            if (!onAuth_(host_, app_, rawKey, false, &reason)) {
                LOG_WARN("RTMP play denied: app=%s key=%s host=%s reason=%s",
                         app_.c_str(), streamKey_.c_str(), host_.c_str(),
                         reason.c_str());
                onStatus(kDefaultStreamId, "error", "NetStream.Play.Failed",
                         reason.empty() ? "play denied" : reason);
                close();
                return;
            }
        }

        playing_ = true;
        sendStreamBegin(kDefaultStreamId);
        onStatus(kDefaultStreamId, "status", "NetStream.Play.Start",
                 "Start playing " + streamKey_);
        if (onPlay_) {
            onPlay_(shared_from_this(), app_, streamKey_);
        }
    } else if (cmd == "deleteStream") {
        close();
    } else if (cmd == "_checkbandwidth" || cmd == "getStreamLength") {
        // 忽略
    }
}

void RtmpConnection::handleData(const RtmpMessage& msg) {
    Buffer buf;
    buf.append(reinterpret_cast<const char*>(msg.payload.data()),
               msg.payload.size());
    Amf0Value first;
    if (!first.decode(buf)) return;
    std::string kind = first.asString();
    if (kind == "@setDataFrame") {
        Amf0Value nameVal;
        if (!nameVal.decode(buf)) return;
        std::string name = nameVal.asString();
        if (name == "onMetaData") {
            Amf0Value meta;
            if (meta.decode(buf) && meta.asObject()) {
                if (stream_) {
                    stream_->setMetadata(*meta.asObject());
                }
            }
        }
    }
}

void RtmpConnection::handleAudio(const RtmpMessage& msg) {
    if (!publishing_ || !stream_ || msg.payload.empty()) return;
    const uint8_t* d = msg.payload.data();
    size_t n = msg.payload.size();
    MediaPacket pkt;
    pkt.type = MediaPacket::kAudio;
    pkt.timestamp = msg.timestamp;
    pkt.data = msg.payload;
    pkt.isKeyframe = true;

    if (EnhancedRtmp::isExHeader(d[0]) && n >= 6) {
        EnhancedRtmp::AudioInfo ai;
        if (EnhancedRtmp::parseAudio(d, n, ai)) {
            pkt.enhanced = true;
            pkt.audioCodec = ai.codec;
            pkt.enhancedPacketType = ai.packetType;
            pkt.isSequenceHeader = (ai.packetType == 0);
        }
    } else {
        pkt.audioCodec = AudioCodec::kAac;
        pkt.isSequenceHeader =
            AacParser::isSequenceHeader(d, n);
    }
    if (pkt.isSequenceHeader) {
        stream_->setAudioSequenceHeader(msg.payload);
    }
    stream_->touch(Timestamp::now().microSecondsSinceEpoch() / 1000);
    stream_->onPacket(std::move(pkt));
}

void RtmpConnection::handleVideo(const RtmpMessage& msg) {
    if (!publishing_ || !stream_ || msg.payload.empty()) return;
    const uint8_t* d = msg.payload.data();
    size_t n = msg.payload.size();
    MediaPacket pkt;
    pkt.type = MediaPacket::kVideo;
    pkt.timestamp = msg.timestamp;
    pkt.data = msg.payload;

    if (EnhancedRtmp::isExHeader(d[0]) && n >= 6) {
        EnhancedRtmp::VideoInfo vi;
        if (EnhancedRtmp::parseVideo(d, n, vi)) {
            pkt.enhanced = true;
            pkt.videoCodec = vi.codec;
            pkt.enhancedPacketType = vi.packetType;
            pkt.isSequenceHeader =
                (vi.packetType == 0 || vi.packetType == 3);
            pkt.isKeyframe = vi.frameType == 1;
            // enhanced 的 packetType 1 = coded frames，关键帧由 frameType 决定
            if (pkt.videoCodec == VideoCodec::kHevc && !pkt.isSequenceHeader) {
                std::vector<uint8_t> au =
                    HevcParser::hvccToAnnexB(vi.payload, vi.payloadLen, 3);
                if (!au.empty() && HevcParser::isKeyframe(au.data(), au.size()))
                    pkt.isKeyframe = true;
            }
        }
    } else {
        pkt.videoCodec = VideoCodec::kH264;
        pkt.isKeyframe = AvcParser::isKeyframe(d, n);
        pkt.isSequenceHeader = AvcParser::isSequenceHeader(d, n);
    }
    if (pkt.isSequenceHeader) {
        stream_->setVideoSequenceHeader(msg.payload);
    }
    stream_->touch(Timestamp::now().microSecondsSinceEpoch() / 1000);
    stream_->onPacket(std::move(pkt));
}

void RtmpConnection::respondConnect(double transactionId,
                                     const std::string& app) {
    // 1) Window Acknowledgement Size
    sendWindowAckSize(kWindowAckSize);
    // 2) Set Peer Bandwidth
    sendSetPeerBandwidth(kPeerBandwidth);
    // 3) _result for connect
    Amf0Value::Object props;
    props["fmsVer"] = Amf0Value::str("FMS/3,0,1,123");
    props["capabilities"] = Amf0Value::number(31.0);
    props["mode"] = Amf0Value::number(1.0);

    Amf0Value::Object info;
    info["level"] = Amf0Value::str("status");
    info["code"] = Amf0Value::str("NetConnection.Connect.Success");
    info["description"] = Amf0Value::str("Connection succeeded.");
    info["objectEncoding"] = Amf0Value::number(0.0);

    RtmpMessage resp;
    resp.typeId = kMsgCommandAmf0;
    resp.chunkStreamId = kCsIdInvoke;
    resp.streamId = 0;
    Buffer pb;
    Amf0Value::str("_result").encode(pb);
    Amf0Value::number(transactionId).encode(pb);
    Amf0Value::object(props).encode(pb);
    Amf0Value::object(info).encode(pb);
    resp.payload.assign(pb.peek(), pb.peek() + pb.readableBytes());
    sendMessage(resp);

    (void)app;
}

void RtmpConnection::respondCreateStream(double transactionId) {
    RtmpMessage resp;
    resp.typeId = kMsgCommandAmf0;
    resp.chunkStreamId = kCsIdInvoke;
    resp.streamId = 0;
    Buffer pb;
    Amf0Value::str("_result").encode(pb);
    Amf0Value::number(transactionId).encode(pb);
    Amf0Value::null().encode(pb);
    Amf0Value::number(static_cast<double>(kDefaultStreamId)).encode(pb);
    resp.payload.assign(pb.peek(), pb.peek() + pb.readableBytes());
    sendMessage(resp);
}

void RtmpConnection::sendWindowAckSize(uint32_t size) {
    RtmpMessage m;
    m.typeId = kMsgWindowAckSize;
    m.chunkStreamId = kCsIdControl;
    m.timestamp = 0;
    m.streamId = 0;
    uint8_t b[4] = {static_cast<uint8_t>((size >> 24) & 0xFF),
                    static_cast<uint8_t>((size >> 16) & 0xFF),
                    static_cast<uint8_t>((size >> 8) & 0xFF),
                    static_cast<uint8_t>(size & 0xFF)};
    m.payload.assign(b, b + 4);
    sendMessage(m);
}

void RtmpConnection::sendSetPeerBandwidth(uint32_t size) {
    RtmpMessage m;
    m.typeId = kMsgSetPeerBandwidth;
    m.chunkStreamId = kCsIdControl;
    m.timestamp = 0;
    m.streamId = 0;
    uint8_t b[5] = {static_cast<uint8_t>((size >> 24) & 0xFF),
                    static_cast<uint8_t>((size >> 16) & 0xFF),
                    static_cast<uint8_t>((size >> 8) & 0xFF),
                    static_cast<uint8_t>(size & 0xFF),
                    2 /* dynamic */};
    m.payload.assign(b, b + 5);
    sendMessage(m);
}

void RtmpConnection::sendSetChunkSize(uint32_t size) {
    RtmpMessage m;
    m.typeId = kMsgSetChunkSize;
    m.chunkStreamId = kCsIdControl;
    m.timestamp = 0;
    m.streamId = 0;
    uint32_t v = size & 0x7FFFFFFF;
    uint8_t b[4] = {static_cast<uint8_t>((v >> 24) & 0xFF),
                    static_cast<uint8_t>((v >> 16) & 0xFF),
                    static_cast<uint8_t>((v >> 8) & 0xFF),
                    static_cast<uint8_t>(v & 0xFF)};
    m.payload.assign(b, b + 4);
    sendMessage(m);
}

void RtmpConnection::sendStreamBegin(uint32_t streamId) {
    RtmpMessage m;
    m.typeId = kMsgUserControlMessage;
    m.chunkStreamId = kCsIdControl;
    m.timestamp = 0;
    m.streamId = 0;
    uint8_t b[6];
    b[0] = 0x00;
    b[1] = 0x00;  // event type = StreamBegin(0)
    b[2] = static_cast<uint8_t>((streamId >> 24) & 0xFF);
    b[3] = static_cast<uint8_t>((streamId >> 16) & 0xFF);
    b[4] = static_cast<uint8_t>((streamId >> 8) & 0xFF);
    b[5] = static_cast<uint8_t>(streamId & 0xFF);
    m.payload.assign(b, b + 6);
    sendMessage(m);
}

void RtmpConnection::onStatus(uint32_t streamId, const std::string& level,
                               const std::string& code,
                               const std::string& description) {
    Amf0Value::Object info;
    info["level"] = Amf0Value::str(level);
    info["code"] = Amf0Value::str(code);
    info["description"] = Amf0Value::str(description);

    RtmpMessage resp;
    resp.typeId = kMsgCommandAmf0;
    resp.chunkStreamId = kCsIdInvoke;
    resp.streamId = streamId;
    Buffer pb;
    Amf0Value::str("onStatus").encode(pb);
    Amf0Value::number(0.0).encode(pb);
    Amf0Value::null().encode(pb);
    Amf0Value::object(info).encode(pb);
    resp.payload.assign(pb.peek(), pb.peek() + pb.readableBytes());
    sendMessage(resp);
}

void RtmpConnection::sendMessage(const RtmpMessage& msg) {
    auto c = conn_.lock();
    if (!c || !c->connected()) return;
    Buffer out;
    chunkWriter_.write(out, msg);
    c->send(&out);
}

void RtmpConnection::splitStreamKey(const std::string& raw, std::string* key,
                                    std::string* query) {
    auto q = raw.find('?');
    *key = raw.substr(0, q);
    if (query) *query = (q == std::string::npos) ? "" : raw.substr(q + 1);
}

void RtmpConnection::startPlayback(const std::shared_ptr<MediaStream>& ms) {
    if (!ms || closed_) return;
    stream_ = ms;

    // onMetaData（data message, type 18）
    auto meta = ms->metadata();
    if (!meta.empty()) {
        Buffer pb;
        Amf0Value::str("onMetaData").encode(pb);
        Amf0Value::ecmaArray(meta).encode(pb);
        RtmpMessage m;
        m.typeId = kMsgDataAmf0;
        m.chunkStreamId = 5;
        m.streamId = kDefaultStreamId;
        m.timestamp = 0;
        m.payload.assign(pb.peek(), pb.peek() + pb.readableBytes());
        sendMessage(m);
    }

    // 订阅回调可能运行在发布者线程：只捕获 weak 引用，
    // ChunkWriter::write 无状态，TcpConnection::send 跨线程安全。
    std::weak_ptr<RtmpConnection> weak = shared_from_this();
    auto flow = std::make_shared<FlowControl>();
    subscriberId_ = ms->subscribeWithGop([weak, flow](const MediaPacket& pkt) {
        auto self = weak.lock();
        if (!self || self->closed_) return;
        auto c = self->conn_.lock();
        if (!c) return;
        switch (flow->check(pkt, c->pendingOutputBytes())) {
            case FlowControl::kDrop:
                return;
            case FlowControl::kClose:
                LOG_WARN("rtmp play: slow client, backlog %zu bytes, closing %s",
                         c->pendingOutputBytes(), c->name().c_str());
                c->forceClose();
                return;
            case FlowControl::kSend:
                break;
        }
        RtmpMessage m;
        m.typeId = static_cast<uint8_t>(pkt.type);
        m.chunkStreamId = pkt.type == MediaPacket::kAudio ? 4 : 6;
        m.streamId = kDefaultStreamId;
        m.timestamp = pkt.timestamp;
        m.payload = pkt.data;
        self->sendMessage(m);
    });
}

void RtmpConnection::rejectPlay(const std::string& code,
                                const std::string& desc) {
    onStatus(kDefaultStreamId, "error", code, desc);
    close();
}

void RtmpConnection::close() {
    if (closed_) return;  // 幂等
    closed_ = true;
    if (stream_ && subscriberId_ != 0) {
        stream_->unsubscribe(subscriberId_);
        subscriberId_ = 0;
    }
    auto self = shared_from_this();  // 回调可能释放外部引用，先保活
    auto c = conn_.lock();
    if (c) c->forceClose();
    if (onClose_) onClose_(self);
}

}  // namespace streaming
