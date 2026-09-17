#pragma once

#include "core/Buffer.hpp"
#include "rtmp/Amf0.hpp"
#include "rtmp/MediaStream.hpp"

#include <cstdint>
#include <functional>

namespace streaming {

/**
 * FlvMuxer: FLV / HTTP-FLV 封装。
 *
 * FLV 文件/流格式：
 *   FLV header (9 字节) + PreviousTagSize0(4) + Tag...
 *   每个 Tag: tag type(1) + data size(3) + timestamp(3+1) + stream id(3)
 *             + data + PreviousTagSize(4)
 *
 * tag type：8=audio，9=video，18=script(metadata)。
 *
 * 本类只做无状态封装，输出到 Buffer。
 */
class FlvMuxer {
public:
    /// 写 FLV 文件头（9 字节）+ PreviousTagSize0
    static void writeHeader(Buffer& out, bool hasAudio = true,
                            bool hasVideo = true);

    /// 写 script tag（onMetaData）
    static void writeScriptTag(Buffer& out, const Amf0Value::Object& meta);

    /// 写一帧媒体 tag
    static void writeMediaTag(Buffer& out, const MediaPacket& pkt);

    /// 通用 tag 写入
    static void writeTag(Buffer& out, uint8_t tagType, uint32_t timestamp,
                         const uint8_t* data, size_t len);

    static constexpr size_t kFlvHeaderSize = 9;
};

}  // namespace streaming
