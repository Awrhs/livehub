#pragma once

#include "rtmp/MediaStream.hpp"

#include <cstddef>
#include <cstdint>

namespace streaming {

/**
 * FlowControl: 单个观众的下行流控（慢客户端保护）。
 *
 * 由媒体分发回调在发送每个包前调用 check(pkt, pendingBytes)：
 *  - 积压 >= hardLimit：返回 kClose，调用方应断开该观众（防止耗尽内存）
 *  - 积压 >= softLimit：进入丢帧模式，丢弃音视频包；
 *    直到遇到视频关键帧且积压回落到 softLimit/2 以下才恢复（保证从 IDR 起解码不花屏）
 *  - sequence header 永不丢弃
 *
 * 线程模型：同一路流的分发在 MediaStream 分发锁下串行执行，无需额外加锁。
 */
class FlowControl {
public:
    enum Action { kSend, kDrop, kClose };

    static constexpr size_t kDefaultSoftLimit = 4 * 1024 * 1024;
    static constexpr size_t kDefaultHardLimit = 32 * 1024 * 1024;

    explicit FlowControl(size_t softLimit = kDefaultSoftLimit,
                         size_t hardLimit = kDefaultHardLimit)
        : soft_(softLimit), hard_(hardLimit) {}

    Action check(const MediaPacket& pkt, size_t pending) {
        if (pending >= hard_) return kClose;
        if (pkt.isSequenceHeader) return kSend;
        if (dropping_) {
            if (pkt.type == MediaPacket::kVideo && pkt.isKeyframe &&
                pending < soft_ / 2) {
                dropping_ = false;
                return kSend;
            }
            ++dropped_;
            return kDrop;
        }
        if (pending >= soft_) {
            dropping_ = true;
            ++dropped_;
            return kDrop;
        }
        return kSend;
    }

    bool dropping() const { return dropping_; }
    uint64_t dropped() const { return dropped_; }

private:
    size_t soft_;
    size_t hard_;
    bool dropping_ = false;
    uint64_t dropped_ = 0;
};

}  // namespace streaming
