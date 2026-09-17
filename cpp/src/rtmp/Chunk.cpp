#include "rtmp/Chunk.hpp"
#include "core/Logger.hpp"

#include <cstring>

namespace streaming {

ChunkParser::ChunkParser(uint32_t chunkSize) : chunkSize_(chunkSize) {}

void ChunkParser::reset() {
    contexts_.clear();
}

bool ChunkParser::parseMessage(Buffer& buf, RtmpMessage& out) {
    // 循环：若当前有未完成消息，先继续读它的后续 chunk；否则读新 chunk 头。
    while (buf.readableBytes() > 0) {
        // 先解析 basic header（fmt + csid），但暂不消费
        if (buf.readableBytes() < 1) return false;
        const uint8_t* hp =
            reinterpret_cast<const uint8_t*>(buf.peek());
        uint8_t fmt = (hp[0] >> 6) & 0x03;
        uint32_t csidLow = hp[0] & 0x3F;
        size_t basicLen = 1;
        uint32_t csid = csidLow;
        if (csidLow == 0) {
            if (buf.readableBytes() < 2) return false;
            basicLen = 2;
            csid = 64 + hp[1];
        } else if (csidLow == 1) {
            if (buf.readableBytes() < 3) return false;
            basicLen = 3;
            csid = 64 + hp[1] + (static_cast<uint32_t>(hp[2]) << 8);
        }

        CsCtx* ctx = nullptr;
        if (fmt == 3) {
            // 继续上一条消息，需要确定 csid（basic header 仍存在）
            auto it = contexts_.find(csid);
            if (it == contexts_.end() || !it->second.inProgress) return false;
            ctx = &it->second;
        } else {
            size_t msgHeaderLen =
                (fmt == 0 ? 11 : fmt == 1 ? 7 : 3);
            if (buf.readableBytes() < basicLen + msgHeaderLen) return false;
            buf.retrieve(basicLen);  // 消费 basic header

            ctx = &contexts_[csid];
            const char* p = buf.peek();
            if (fmt == 0) {
                uint32_t ts = (static_cast<uint8_t>(p[0]) << 16) |
                              (static_cast<uint8_t>(p[1]) << 8) |
                              static_cast<uint8_t>(p[2]);
                uint32_t len = (static_cast<uint8_t>(p[3]) << 16) |
                               (static_cast<uint8_t>(p[4]) << 8) |
                               static_cast<uint8_t>(p[5]);
                uint8_t typeId = static_cast<uint8_t>(p[6]);
                uint32_t streamId =
                    static_cast<uint32_t>(static_cast<uint8_t>(p[7])) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(p[8])) << 8) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(p[9])) << 16) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(p[10])) << 24);
                buf.retrieve(11);
                ctx->timestamp = ts;
                ctx->msgLength = len;
                ctx->typeId = typeId;
                ctx->streamId = streamId;
                ctx->bytesRead = 0;
                ctx->payload.clear();
                ctx->payload.reserve(len);
                ctx->inProgress = true;
                ctx->hasExtendedTimestamp = (ts >= 0xFFFFFF);
            } else if (fmt == 1) {
                uint32_t delta = (static_cast<uint8_t>(p[0]) << 16) |
                                 (static_cast<uint8_t>(p[1]) << 8) |
                                 static_cast<uint8_t>(p[2]);
                uint32_t len = (static_cast<uint8_t>(p[3]) << 16) |
                               (static_cast<uint8_t>(p[4]) << 8) |
                               static_cast<uint8_t>(p[5]);
                uint8_t typeId = static_cast<uint8_t>(p[6]);
                buf.retrieve(7);
                ctx->timestamp += delta;
                ctx->msgLength = len;
                ctx->typeId = typeId;
                ctx->bytesRead = 0;
                ctx->payload.clear();
                ctx->payload.reserve(len);
                ctx->inProgress = true;
                ctx->hasExtendedTimestamp = (delta >= 0xFFFFFF);
            } else {  // fmt == 2
                uint32_t delta = (static_cast<uint8_t>(p[0]) << 16) |
                                 (static_cast<uint8_t>(p[1]) << 8) |
                                 static_cast<uint8_t>(p[2]);
                buf.retrieve(3);
                ctx->timestamp += delta;
                ctx->bytesRead = 0;
                ctx->payload.clear();
                ctx->payload.reserve(ctx->msgLength);
                ctx->inProgress = true;
                ctx->hasExtendedTimestamp = (delta >= 0xFFFFFF);
            }
        }

        // 扩展时间戳（fmt 0/1/2 在 message header 后；fmt 3 续传 chunk 也可能有）
        bool needExtTs = ctx->hasExtendedTimestamp;
        if (needExtTs) {
            // 仅在新消息的第一个 chunk 或明确要求时读 4 字节
            if (fmt != 3 || ctx->bytesRead == 0) {
                if (buf.readableBytes() < 4) return false;
                uint32_t ext = (static_cast<uint32_t>(
                                    static_cast<uint8_t>(buf.peek()[0])) << 24) |
                               (static_cast<uint32_t>(
                                    static_cast<uint8_t>(buf.peek()[1])) << 16) |
                               (static_cast<uint32_t>(
                                    static_cast<uint8_t>(buf.peek()[2])) << 8) |
                               static_cast<uint32_t>(
                                   static_cast<uint8_t>(buf.peek()[3]));
                buf.retrieve(4);
                if (ext > 0) ctx->timestamp = ext;
            }
        }

        // fmt 3：basic header 还没消费
        if (fmt == 3) {
            buf.retrieve(basicLen);
        }

        // 读 payload 片段
        size_t remaining = ctx->msgLength - ctx->bytesRead;
        size_t toRead = remaining < chunkSize_ ? remaining : chunkSize_;
        if (buf.readableBytes() < toRead) return false;

        const char* p = buf.peek();
        ctx->payload.insert(ctx->payload.end(),
                             reinterpret_cast<const uint8_t*>(p),
                             reinterpret_cast<const uint8_t*>(p) + toRead);
        buf.retrieve(toRead);
        ctx->bytesRead += static_cast<uint32_t>(toRead);

        if (ctx->bytesRead >= ctx->msgLength) {
            out.typeId = ctx->typeId;
            out.timestamp = ctx->timestamp;
            out.streamId = ctx->streamId;
            out.payload = std::move(ctx->payload);
            ctx->payload.clear();
            ctx->inProgress = false;
            return true;
        }
        // 否则继续读下一个续传 chunk（fmt 3）
    }
    return false;
}

// ---- ChunkWriter ----
static void writeBasicHeader(Buffer& out, uint8_t fmt, uint32_t csid) {
    uint8_t b0 = static_cast<uint8_t>((fmt << 6) | (csid & 0x3F));
    out.appendInt8(static_cast<int8_t>(b0));
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
