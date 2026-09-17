#pragma once

#include "core/NonCopyable.hpp"
#include "hls/Segmenter.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace streaming {

/**
 * HlsSource: 一路直播流的 HLS 切片状态。
 *
 * 接收编码后的音视频帧（AVC Annex-B / AAC ADTS），交给 TsMuxer→Segmenter，
 * 在关键帧处按时长滚动分片。
 *
 * 线程模型：所有 onVideoFrame/onAudioFrame 应在同一线程（或 strand）调用。
 */
class HlsSource : NonCopyable {
public:
    HlsSource(std::string streamKey, std::string outputDir,
              int targetDuration = 6, int windowSize = 5);
    ~HlsSource();

    bool start();

    /// 注入异步文件写线程（创建后、start 前调用）
    void setAsyncWriter(class AsyncFileWriter* w);

    /// 设置视频流类型（kStreamTypeVideo AVC / kStreamTypeHevc HEVC）
    void setVideoStreamType(uint8_t t);

    /// 写入一帧视频（Annex-B，含 SPS/PPS 的 IDR 会触发分片边界）
    void onVideoFrame(const uint8_t* data, size_t len, int64_t pts90k,
                      int64_t dts90k, bool isKeyframe);

    /// 写入一帧音频（ADTS）
    void onAudioFrame(const uint8_t* data, size_t len, int64_t pts90k);

    std::string playlist();
    std::string segmentPath(uint64_t seq) const;

    const std::string& key() const { return streamKey_; }
    uint64_t currentSequence() const { return segmenter_->currentSequence(); }
    std::string outputDir() const { return segmenter_->outputDir(); }

    /// 活跃时间戳，用于过期回收
    int64_t lastActiveMs() const { return lastActiveMs_.load(); }

private:
    std::string streamKey_;
    std::unique_ptr<Segmenter> segmenter_;
    class AsyncFileWriter* asyncWriter_ = nullptr;
    int targetDuration_;
    int64_t segmentStartDts_ = -1;
    int64_t lastDts_ = -1;
    std::atomic<int64_t> lastActiveMs_{0};
};

}  // namespace streaming
