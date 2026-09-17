#include "hls/HlsSource.hpp"
#include "core/Logger.hpp"

#include <chrono>

namespace streaming {

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

HlsSource::HlsSource(std::string streamKey, std::string outputDir,
                     int targetDuration, int windowSize)
    : streamKey_(std::move(streamKey)),
      targetDuration_(targetDuration) {
    segmenter_ = std::make_unique<Segmenter>(
        std::move(outputDir), streamKey_, targetDuration, windowSize);
}

HlsSource::~HlsSource() = default;

bool HlsSource::start() {
    if (asyncWriter_) segmenter_->setAsyncWriter(asyncWriter_);
    return segmenter_->open();
}

void HlsSource::setAsyncWriter(AsyncFileWriter* w) {
    asyncWriter_ = w;
    segmenter_->setAsyncWriter(w);
}

void HlsSource::setVideoStreamType(uint8_t t) {
    segmenter_->muxer().setVideoStreamType(t);
}

void HlsSource::onVideoFrame(const uint8_t* data, size_t len,
                              int64_t pts90k, int64_t dts90k,
                              bool isKeyframe) {
    lastActiveMs_.store(nowMs());

    // 关键帧且当前分片已达目标时长，滚动分片
    if (isKeyframe) {
        if (segmentStartDts_ < 0) {
            segmentStartDts_ = dts90k;
        } else {
            double elapsedSec =
                static_cast<double>(dts90k - segmentStartDts_) / 90000.0;
            if (elapsedSec >= targetDuration_) {
                segmenter_->newSegment(dts90k);
                segmentStartDts_ = dts90k;
            }
        }
    }

    segmenter_->muxer().writeVideo(data, len, pts90k, dts90k, isKeyframe);
    lastDts_ = dts90k;
}

void HlsSource::onAudioFrame(const uint8_t* data, size_t len,
                              int64_t pts90k) {
    lastActiveMs_.store(nowMs());
    segmenter_->muxer().writeAudio(data, len, pts90k);
}

std::string HlsSource::playlist() {
    return segmenter_->playlist();
}

std::string HlsSource::segmentPath(uint64_t seq) const {
    return segmenter_->segmentPath(seq);
}

}  // namespace streaming
