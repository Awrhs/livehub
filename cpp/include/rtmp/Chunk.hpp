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

    /// 对端 Set Chunk Size；非法值（0 或 > 0x7FFFFFFF）忽略
    void setChunkSize(uint32_t size) {
        if (size >= 1 && size <= 0x7FFFFFFF) chunkSize_ = size;
    }
    uint32_t chunkSize() const { return chunkSize_; }

    /// 从 buffer 解析一条完整消息。
    /// 只有当一个完整 chunk（头 + 扩展时间戳 + 本片负载）都已到达时才整体消费，
    /// 半包时 buffer 原样保留，可安全地在后续数据到达后重入。
    /// @return true 表示 out 已填充完整消息；false 表示数据不足或出错（见 hasError）。
    bool parseMessage(Buffer& buf, RtmpMessage& out);

    /// 协议错误（非法 fmt3 起始、超大消息等），连接应关闭
    bool hasError() const { return error_; }

    void reset();

    /// 单条消息上限（RTMP 长度字段 24 位，最大 16MB）；分配按实际到达增长
    static constexpr uint32_t kMaxMessageSize = 0xFFFFFF;
    /// 同时活跃的 chunk stream 上限，防止恶意 csid 洪泛耗尽内存
    static constexpr size_t kMaxChunkStreams = 64;

private:
    struct CsCtx {
        uint32_t timestamp = 0;      // 当前消息绝对时间戳
        uint32_t timestampDelta = 0; // 最近一次 delta（fmt3 起新消息时复用）
        uint32_t msgLength = 0;
        uint8_t typeId = 0;
        uint32_t streamId = 0;
        uint32_t bytesRead = 0;
        std::vector<uint8_t> payload;
        bool inProgress = false;
        bool hasHeader = false;          // 是否收到过 fmt0/1/2 头
        bool hasExtendedTimestamp = false;
        uint32_t extTimestamp = 0;       // 最近一次扩展时间戳原值
    };

    uint32_t chunkSize_;
    bool error_ = false;
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
