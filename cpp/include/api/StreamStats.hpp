#pragma once

#include "core/NonCopyable.hpp"
#include "rtmp/MediaStream.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace streaming {

/**
 * StreamStats: 单路流的运行时统计。
 *
 * - 字节/包计数（音视频分别统计）
 * - 码率（滑动窗口，按最近 N 秒）
 * - 观众数（订阅者数）
 * - 在线时长
 * - 分辨率（从 SPS 解析，可选）
 */
class StreamStats : NonCopyable {
public:
    explicit StreamStats(std::string key);

    void onVideoPacket(size_t bytes);
    void onAudioPacket(size_t bytes);

    /// 注册/注销一个观众（播放端）
    void addViewer() { viewers_.fetch_add(1, std::memory_order_relaxed); }
    void removeViewer() { viewers_.fetch_sub(1, std::memory_order_relaxed); }
    int64_t viewers() const { return viewers_.load(std::memory_order_relaxed); }

    void setVideoInfo(int width, int height, int fps) {
        width_ = width;
        height_ = height;
        fps_ = fps;
    }
    void setAudioInfo(int sampleRate, int channels) {
        audioSampleRate_ = sampleRate;
        audioChannels_ = channels;
    }
    void setBitrateVideo(int br) { videoBitrate_ = br; }
    void setBitrateAudio(int br) { audioBitrate_ = br; }

    std::string key() const { return key_; }
    int64_t startedSec() const;
    int64_t videoBytes() const { return videoBytes_.load(); }
    int64_t audioBytes() const { return audioBytes_.load(); }
    int64_t videoPackets() const { return videoPackets_.load(); }
    int64_t audioPackets() const { return audioPackets_.load(); }
    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }
    int audioSampleRate() const { return audioSampleRate_; }
    int audioChannels() const { return audioChannels_; }
    int videoBitrate() const { return videoBitrate_; }
    int audioBitrate() const { return audioBitrate_; }

private:
    std::string key_;
    int64_t startedAtSec_;
    std::atomic<int64_t> videoBytes_{0};
    std::atomic<int64_t> audioBytes_{0};
    std::atomic<int64_t> videoPackets_{0};
    std::atomic<int64_t> audioPackets_{0};
    std::atomic<int64_t> viewers_{0};
    int width_ = 0, height_ = 0, fps_ = 0;
    int audioSampleRate_ = 0, audioChannels_ = 0;
    int videoBitrate_ = 0, audioBitrate_ = 0;
};

/**
 * StatsRegistry: 所有流的统计注册表。
 *
 * 包级私有单例（或显式持有）。RTMP 推流/桥接器在 onPacket 时更新；
 * HTTP-FLV/HLS/WebRTC 播放时 addViewer/removeViewer。
 */
class StatsRegistry : NonCopyable {
public:
    static StatsRegistry& instance();

    std::shared_ptr<StreamStats> getOrCreate(const std::string& key);
    std::shared_ptr<StreamStats> get(const std::string& key) const;
    void remove(const std::string& key);
    std::map<std::string, std::shared_ptr<StreamStats>> all() const;

private:
    StatsRegistry() = default;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<StreamStats>> stats_;
};

}  // namespace streaming
