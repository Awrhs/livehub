#pragma once

#include "core/NonCopyable.hpp"
#include "rtmp/Amf0.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

enum class VideoCodec { kUnknown = 0, kH264 = 7, kHevc = 0x68766331 /*'hvc1'*/ };
enum class AudioCodec { kUnknown = 0, kAac = 10, kOpus = 11, kMp3 = 2 };

/**
 * 编码后的媒体包（RTMP 原始音视频消息，含 Enhanced RTMP 扩展）。
 */
struct MediaPacket {
    enum Type { kAudio = 8, kVideo = 9, kScript = 18 };
    Type type;
    uint32_t timestamp;
    std::vector<uint8_t> data;
    bool isKeyframe = false;
    bool isSequenceHeader = false;

    // 视频编解码（Enhanced RTMP 下为 hvc1/av01；legacy 为 H.264）
    VideoCodec videoCodec = VideoCodec::kUnknown;
    AudioCodec audioCodec = AudioCodec::kUnknown;
    // Enhanced RTMP：是否为扩展消息头
    bool enhanced = false;
    // Enhanced RTMP 包类型：0=sequence start,1=coded frames,2=coded frames X,3=codec config
    uint8_t enhancedPacketType = 0;
};

/**
 * MediaStream: 一路直播流的媒体数据与状态。
 *
 * 维护：
 * - metadata（onMetaData，AMF0 object）
 * - 视频/音频 sequence header（AVCDecoderConfigurationRecord / ASC）
 * - GOP 缓存（从最近关键帧起的音视频包），供新观众快速起播
 *
 * 线程安全：内部加锁，发布者写、多个播放者/桥接器读。
 */
class MediaStream : public std::enable_shared_from_this<MediaStream> {
public:
    using PacketCallback = std::function<void(const MediaPacket&)>;
    using SubscriberId = uint64_t;

    explicit MediaStream(std::string streamKey, std::string app = "live");

    void setMetadata(Amf0Value::Object meta) {
        std::lock_guard<std::mutex> lk(mu_);
        metadata_ = std::move(meta);
    }
    Amf0Value::Object metadata() const {
        std::lock_guard<std::mutex> lk(mu_);
        return metadata_;
    }

    void setVideoSequenceHeader(std::vector<uint8_t> asc) {
        std::lock_guard<std::mutex> lk(mu_);
        videoSeqHeader_ = std::move(asc);
    }
    void setAudioSequenceHeader(std::vector<uint8_t> asc) {
        std::lock_guard<std::mutex> lk(mu_);
        audioSeqHeader_ = std::move(asc);
    }
    std::vector<uint8_t> videoSequenceHeader() const {
        std::lock_guard<std::mutex> lk(mu_);
        return videoSeqHeader_;
    }
    std::vector<uint8_t> audioSequenceHeader() const {
        std::lock_guard<std::mutex> lk(mu_);
        return audioSeqHeader_;
    }

    /// 收到一帧：更新 GOP 缓存并通知订阅者
    void onPacket(MediaPacket pkt);

    /// 订阅实时包；返回 id，可用于取消
    SubscriberId subscribe(PacketCallback cb);

    /// 原子地“回放 sequence header + GOP 缓存，然后订阅实时包”。
    /// 回放与注册在同一把分发锁下完成，保证新观众收到的包不丢、不重、不乱序。
    /// 回调可能在发布者线程中被调用，必须轻量且线程安全（例如只做 conn->send）。
    SubscriberId subscribeWithGop(PacketCallback cb);
    void unsubscribe(SubscriberId id);

    /// 把 GOP 缓存（sequence header + 最近关键帧起的包）推给回调
    void playGop(const PacketCallback& cb) const;

    const std::string& key() const { return streamKey_; }
    const std::string& app() const { return app_; }
    const std::string& vhost() const { return vhost_; }
    void setVhost(std::string v) { vhost_ = std::move(v); }
    bool hasPublisher() const { return hasPublisher_.load(); }
    void setHasPublisher(bool on) { hasPublisher_.store(on); }
    uint32_t subscriberCount() const;

    /// 最近活跃时间（ms）
    int64_t lastActiveMs() const { return lastActiveMs_.load(); }
    void touch(int64_t ms) { lastActiveMs_.store(ms); }

private:
    std::string streamKey_;
    std::string app_;
    std::string vhost_;

    mutable std::mutex mu_;
    Amf0Value::Object metadata_;
    std::vector<uint8_t> videoSeqHeader_;
    std::vector<uint8_t> audioSeqHeader_;
    std::deque<MediaPacket> gop_;     // 从最近 IDR 起的音视频包（不含 seq header）
    bool gopHasKeyframe_ = false;
    size_t gopMaxPackets_ = 4096;    // 超过即整体丢弃，等待下一个关键帧

    // 分发锁：onPacket 的“更新 GOP + 通知订阅者”与 subscribeWithGop 互斥，
    // 保证新订阅者的 GOP 回放与实时包严格衔接。
    mutable std::mutex dispatchMu_;

    mutable std::mutex subMu_;
    std::vector<std::pair<SubscriberId, PacketCallback>> subscribers_;
    SubscriberId nextSubId_ = 1;

    std::atomic<bool> hasPublisher_{false};
    std::atomic<int64_t> lastActiveMs_{0};
};

using MediaStreamPtr = std::shared_ptr<MediaStream>;

}  // namespace streaming
