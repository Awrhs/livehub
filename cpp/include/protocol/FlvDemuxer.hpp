#pragma once

#include "core/Buffer.hpp"
#include "rtmp/MediaStream.hpp"

#include <cstdint>
#include <functional>

namespace streaming {

/**
 * FlvDemuxer: 从 HTTP-FLV 字节流解析 FLV tag 并还原 MediaPacket。
 *
 * 状态机：先读 9 字节 FLV header + 4 字节 PreviousTagSize0，
 * 然后每个 tag 读 11 字节头 + data + 4 字节 PreviousTagSize。
 *
 * 支持半包：内部维护累积缓冲，数据不足时保留状态。
 */
class FlvDemuxer {
public:
    using TagCallback = std::function<void(const MediaPacket&)>;

    void setTagCallback(TagCallback cb) { onTag_ = std::move(cb); }

    /// 喂入数据，解析出完整 tag 时回调。
    void feed(const uint8_t* data, size_t len);
    void reset();

    bool headerParsed() const { return headerParsed_; }

private:
    bool parseHeader();
    bool parseTag();

    TagCallback onTag_;
    Buffer buf_;
    bool headerParsed_ = false;
    bool headersSkipped_ = false;  // 是否已跳过 HTTP 响应头
};

}  // namespace streaming
