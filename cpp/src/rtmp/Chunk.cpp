#include "rtmp/Chunk.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cstring>

namespace streaming {

ChunkParser::ChunkParser(uint32_t chunkSize) : chunkSize_(chunkSize) {}

void ChunkParser::reset() {
    contexts_.clear();
    error_ = false;
}

namespace {
inline uint32_t rd24(const uint8_t* p) {
    return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
}
inline uint32_t rd32be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint32_t rd32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
}  // namespace

bool ChunkParser::parseMessage(Buffer& buf, RtmpMessage& out) {
    if (error_) return false;
    // 每次循环处理一个完整 chunk：先只 peek 计算总长度，数据齐了再一次性消费。
    for (;;) {
        const size_t avail = buf.readableBytes();
        if (avail < 1) return false;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.peek());

        // ---- basic header ----
        const uint8_t fmt = (p[0] >> 6) & 0x03;
        uint32_t csid = p[0] & 0x3F;
        size_t off = 1;
        if (csid == 0) {
            if (avail < 2) return false;
            csid = 64 + p[1];
            off = 2;
        } else if (csid == 1) {
            if (avail < 3) return false;
            csid = 64 + p[1] + (uint32_t(p[2]) << 8);
            off = 3;
        }

        auto it = contexts_.find(csid);
        if (it == contexts_.end()) {
            if (fmt != 0 || contexts_.size() >= kMaxChunkStreams) {
                // 新 chunk stream 必须以 fmt0 开始；或 csid 数量超限
                LOG_WARN("rtmp chunk: invalid new csid=%u fmt=%u", csid, fmt);
                error_ = true;
                return false;
            }
        }
        // 只读快照：不在数据不足时修改任何状态
        const CsCtx* prev = (it == contexts_.end()) ? nullptr : &it->second;

        // ---- message header ----
        const size_t mhLen = fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0;
        if (avail < off + mhLen) return false;
        const uint8_t* mh = p + off;
        off += mhLen;

        uint32_t tsField = 0, msgLen = 0, streamId = 0;
        uint8_t typeId = 0;
        if (fmt <= 2) tsField = rd24(mh);
        if (fmt <= 1) {
            msgLen = rd24(mh + 3);
            typeId = mh[6];
        }
        if (fmt == 0) streamId = rd32le(mh + 7);

        const bool continuing = (fmt == 3 && prev && prev->inProgress);
        if (fmt == 3 && (!prev || !prev->hasHeader)) {
            LOG_WARN("rtmp chunk: fmt3 without prior header csid=%u", csid);
            error_ = true;
            return false;
        }

        // ---- extended timestamp ----
        bool hasExt;
        if (fmt <= 2) {
            hasExt = (tsField == 0xFFFFFF);
        } else {
            hasExt = prev->hasExtendedTimestamp;
        }
        uint32_t ext = 0;
        if (hasExt) {
            if (avail < off + 4) return false;
            ext = rd32be(p + off);
            if (continuing) {
                // 续传 chunk 的扩展时间戳：FFmpeg/OBS 会带，部分实现不带。
                // 与本消息的扩展时间戳相同才视为存在（SRS 同款兼容策略）。
                if (ext == prev->extTimestamp) off += 4;
            } else {
                off += 4;
            }
        }

        // ---- 计算本片负载长度 ----
        uint32_t fullLen = (fmt <= 1) ? msgLen : prev->msgLength;
        if (fullLen > kMaxMessageSize) {
            error_ = true;
            return false;
        }
        uint32_t already = continuing ? prev->bytesRead : 0;
        uint32_t remaining = fullLen - already;
        uint32_t piece = remaining < chunkSize_ ? remaining : chunkSize_;
        if (avail < off + piece) return false;

        // ===== 数据齐全：从这里开始才修改状态并消费 =====
        CsCtx& ctx = contexts_[csid];
        if (!continuing) {
            // 新消息开始
            if (fmt == 0) {
                ctx.timestamp = hasExt ? ext : tsField;
                ctx.timestampDelta = 0;
                ctx.msgLength = msgLen;
                ctx.typeId = typeId;
                ctx.streamId = streamId;
            } else if (fmt == 1 || fmt == 2) {
                uint32_t delta = hasExt ? ext : tsField;
                ctx.timestampDelta = delta;
                ctx.timestamp += delta;
                if (fmt == 1) {
                    ctx.msgLength = msgLen;
                    ctx.typeId = typeId;
                }
            } else {  // fmt3 起新消息：沿用上一条的全部头信息与 delta
                ctx.timestamp += ctx.timestampDelta;
            }
            ctx.hasExtendedTimestamp = hasExt;
            if (fmt <= 2) ctx.extTimestamp = ext;
            ctx.hasHeader = true;
            ctx.bytesRead = 0;
            ctx.payload.clear();
            // 不按声明长度一次性 reserve（防伪造长度耗内存），上限 64KB 预留
            ctx.payload.reserve(std::min<uint32_t>(ctx.msgLength, 64 * 1024));
            ctx.inProgress = true;
        }

        const uint8_t* data = p + off;
        ctx.payload.insert(ctx.payload.end(), data, data + piece);
        ctx.bytesRead += piece;
        buf.retrieve(off + piece);

        if (ctx.bytesRead >= ctx.msgLength) {
            out.typeId = ctx.typeId;
            out.timestamp = ctx.timestamp;
            out.streamId = ctx.streamId;
            out.payload = std::move(ctx.payload);
            ctx.payload.clear();
            ctx.inProgress = false;
            return true;
        }
        // 否则继续处理下一个 chunk（可能是其他 csid 交织进来）
    }
}

// ---- ChunkWriter ----
static void writeBasicHeader(Buffer& out, uint8_t fmt, uint32_t csid) {
    // csid 2..63: 1 字节；64..319: 2 字节；320..65599: 3 字节
    if (csid >= 2 && csid <= 63) {
        out.appendInt8(static_cast<int8_t>((fmt << 6) | csid));
    } else if (csid >= 64 && csid <= 319) {
        out.appendInt8(static_cast<int8_t>(fmt << 6));
        out.appendInt8(static_cast<int8_t>(csid - 64));
    } else {
        uint32_t v = csid - 64;
        out.appendInt8(static_cast<int8_t>((fmt << 6) | 1));
        out.appendInt8(static_cast<int8_t>(v & 0xFF));
        out.appendInt8(static_cast<int8_t>((v >> 8) & 0xFF));
    }
}

static void writeExtendedTimestamp(Buffer& out, uint32_t ts) {
    out.appendInt8(static_cast<int8_t>((ts >> 24) & 0xFF));
    out.appendInt8(static_cast<int8_t>((ts >> 16) & 0xFF));
    out.appendInt8(static_cast<int8_t>((ts >> 8) & 0xFF));
    out.appendInt8(static_cast<int8_t>(ts & 0xFF));
}

void ChunkWriter::write(Buffer& out, const RtmpMessage& msg) const {
    const uint8_t* data = msg.payload.data();
    size_t total = msg.payload.size();
    size_t offset = 0;
    bool first = true;

    while (offset < total || first) {
        if (first) {
            writeBasicHeader(out, 0, msg.chunkStreamId);

            uint32_t ts = msg.timestamp;
            bool ext = ts >= 0xFFFFFF;
            uint32_t tsField = ext ? 0xFFFFFF : ts;
            out.appendInt8(static_cast<int8_t>((tsField >> 16) & 0xFF));
            out.appendInt8(static_cast<int8_t>((tsField >> 8) & 0xFF));
            out.appendInt8(static_cast<int8_t>(tsField & 0xFF));

            uint32_t len = static_cast<uint32_t>(total);
            out.appendInt8(static_cast<int8_t>((len >> 16) & 0xFF));
            out.appendInt8(static_cast<int8_t>((len >> 8) & 0xFF));
            out.appendInt8(static_cast<int8_t>(len & 0xFF));

            out.appendInt8(static_cast<int8_t>(msg.typeId));
            out.appendInt8(static_cast<int8_t>(msg.streamId & 0xFF));
            out.appendInt8(static_cast<int8_t>((msg.streamId >> 8) & 0xFF));
            out.appendInt8(static_cast<int8_t>((msg.streamId >> 16) & 0xFF));
            out.appendInt8(static_cast<int8_t>((msg.streamId >> 24) & 0xFF));

            if (ext) writeExtendedTimestamp(out, ts);
        } else {
            writeBasicHeader(out, 3, msg.chunkStreamId);
            // FFmpeg/librtmp 读端在 fmt3 续传 chunk 上同样期待扩展时间戳
            if (msg.timestamp >= 0xFFFFFF) writeExtendedTimestamp(out, msg.timestamp);
        }

        size_t remaining = total - offset;
        size_t chunk = remaining < chunkSize_ ? remaining : chunkSize_;
        if (chunk > 0) {
            out.append(reinterpret_cast<const char*>(data + offset), chunk);
            offset += chunk;
        }
        first = false;
        if (chunk == 0) break;
    }
}

}  // namespace streaming
