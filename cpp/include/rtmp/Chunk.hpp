#pragma once

#include "core/Buffer.hpp"
#include "rtmp/RtmpMessage.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace streaming {

/**
 * ChunkParser: RTMP Chunk 分帧重组器。
 *
 * RTMP 把消息切成 chunk，chunk 头随 fmt 不同而长度不同：
 *   fmt 0 : 11 字节，绝对时间戳、长度、type id、stream id
 *   fmt 1 :  7 字节，时间戳 delta、长度、type id
 *   fmt 2 :  3 字节，时间戳 delta
 *   fmt 3 :  0 字节，沿用上一帧
 * 时间戳 >= 0xFFFFFF 时使用扩展时间戳（4 字节）。
 *
 * 简历亮点：手写二进制分帧状态机，处理半包、跨 chunk 重组
 */
class ChunkParser {
public:
    explicit ChunkParser(uint32_t chunkSize = 128);

    void setChunkSize(uint32_t size) { chunkSize_ = size; }
    uint32_t chunkSize() const { return chunkSize_; }

    /// 从 buffer 解析一条完整消息。
    /// @return true 表示 out 已填充完整消息；false 表示数据不足（状态保留）。
    bool parseMessage(Buffer& buf, RtmpMessage& out);

    void reset();

private:
    struct CsCtx {
        uint32_t timestamp = 0;
        uint32_t msgLength = 0;
        uint8_t typeId = 0;
        uint32_t streamId = 0;
        uint32_t bytesRead = 0;
        std::vector<uint8_t> payload;
        bool inProgress = false;
        bool hasExtendedTimestamp = false;
    };

    uint32_t chunkSize_;
    std::unordered_map<uint32_t, CsCtx> contexts_;
};

/**
 * ChunkWriter: 把完整消息切成 chunk 写入 Buffer。
 *
 * 第一片 fmt=0（完整消息头），后续片 fmt=3。
 * 时间戳 >= 0xFFFFFF 写扩展时间戳。
 */
class ChunkWriter {
public:
    explicit ChunkWriter(uint32_t chunkSize = 128) : chunkSize_(chunkSize) {}
    void setChunkSize(uint32_t s) { chunkSize_ = s; }

    void write(Buffer& out, const RtmpMessage& msg) const;

private:
    uint32_t chunkSize_;
};

}  // namespace streaming
