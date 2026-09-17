#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace streaming {

/**
 * RTMP 协议常量
 */
enum RtmpMessageType : uint8_t {
    kMsgSetChunkSize = 1,
    kMsgAbortMessage = 2,
    kMsgAcknowledgement = 3,
    kMsgUserControlMessage = 4,
    kMsgWindowAckSize = 5,
    kMsgSetPeerBandwidth = 6,
    kMsgAudio = 8,
    kMsgVideo = 9,
    kMsgDataAmf3 = 15,
    kMsgDataAmf0 = 18,
    kMsgCommandAmf3 = 17,
    kMsgCommandAmf0 = 20,
    kMsgAggregate = 22,
};

/**
 * RTMP 常用 chunk stream id
 */
enum : uint32_t {
    kCsIdControl = 2,    // 协议控制
    kCsIdInvoke = 3,     // AMF0 command
    kCsIdAudio = 4,
    kCsIdVideo = 6,
    kCsIdData = 5,
};

/**
 * RtmpMessage: 一个已重组的完整 RTMP 消息。
 */
struct RtmpMessage {
    uint8_t typeId = 0;
    uint32_t chunkStreamId = 0;
    uint32_t timestamp = 0;      // 已解析 delta 后的绝对时间戳
    uint32_t streamId = 0;
    std::vector<uint8_t> payload;

    void clear() {
        typeId = 0;
        chunkStreamId = 0;
        timestamp = 0;
        streamId = 0;
        payload.clear();
    }
};

}  // namespace streaming
