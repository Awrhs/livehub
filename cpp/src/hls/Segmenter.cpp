#include "hls/Segmenter.hpp"
#include "core/AsyncFileWriter.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace streaming {

Segmenter::Segmenter(std::string outputDir, std::string streamName,
                     int targetDuration, int windowSize)
    : outputDir_(std::move(outputDir)),
      streamName_(std::move(streamName)),
      targetDuration_(targetDuration),
      windowSize_(windowSize),
      playlist_(targetDuration, PlaylistType::kLive, windowSize) {
    muxer_ = std::make_unique<TsMuxer>(
        [this](const uint8_t* d, size_t n) { this->writeTs(d, n); });
}

Segmenter::~Segmenter() {
    finalize();
}

bool Segmenter::open() {
    std::string dir = outputDir_ + "/" + streamName_;
    ::mkdir(outputDir_.c_str(), 0755);
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("Segmenter mkdir %s failed: %s", dir.c_str(),
                  strerror(errno));
        return false;
    }
    sequence_ = 0;
    segmentStartPts_ = -1;
    lastPts_ = -1;
    segmentStartWallUs_ = -1;
    // 打开第一个分片
    newSegment(0, false);
    started_ = true;
    return true;
}

std::string Segmenter::segmentPath(uint64_t sequence) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%llu.ts",
                  static_cast<unsigned long long>(sequence));
    return outputDir_ + "/" + streamName_ + "/" + name;
}

void Segmenter::writeTs(const uint8_t* data, size_t len) {
    if (asyncWriter_) {
        if (currentAsyncId_ != 0) {
            asyncWriter_->append(currentAsyncId_, data, len);
        }
        return;
    }
    if (currentFile_.is_open()) {
        currentFile_.write(reinterpret_cast<const char*>(data),
                           static_cast<std::streamsize>(len));
    }
}

void Segmenter::newSegment(int64_t boundaryPts, bool discontinuity) {
    // 关闭上一分片（用 boundaryPts 计算时长）
    if ((currentFile_.is_open() || currentAsyncId_ != 0)) {
        closeCurrentSegment(boundaryPts);
    }

    if (discontinuity) {
        playlist_.setDiscontinuity();
    }

    ++sequence_;
    std::string path = segmentPath(sequence_);
    if (asyncWriter_) {
        currentAsyncId_ = asyncWriter_->open(path, /*append=*/false);
    } else {
        currentFile_.open(path, std::ios::binary | std::ios::trunc);
        if (!currentFile_.is_open()) {
            LOG_ERROR("Segmenter open %s failed: %s", path.c_str(),
                      strerror(errno));
            return;
        }
    }
    segmentStartPts_ = boundaryPts;
    lastPts_ = boundaryPts;
    segmentStartWallUs_ = Timestamp::now().microSecondsSinceEpoch();
    liveSegments_.push_back(sequence_);

    LOG_DEBUG("Segmenter [%s] new segment %llu", streamName_.c_str(),
              static_cast<unsigned long long>(sequence_));
}

void Segmenter::closeCurrentSegment(int64_t endPts) {
    bool open = currentFile_.is_open() || currentAsyncId_ != 0;
    if (currentFile_.is_open()) currentFile_.close();
    if (asyncWriter_ && currentAsyncId_ != 0) {
        uint64_t id = currentAsyncId_;
        currentAsyncId_ = 0;
        asyncWriter_->close(id);
    }
    if (!open) return;

    if (segmentStartPts_ >= 0 && endPts > segmentStartPts_) {
        double duration =
            static_cast<double>(endPts - segmentStartPts_) / 90000.0;
        // 分片 URI 相对 /live/<stream>.m3u8 解析，须带流名目录：<stream>/<seq>.ts
        // （分片路由为 /live/:stream/:segment；原来只写 <seq>.ts 会解析成 /live/<seq>.ts → 404）
        char name[32];
        std::snprintf(name, sizeof(name), "%llu.ts",
                      static_cast<unsigned long long>(sequence_));
        playlist_.addSegment(sequence_, duration, streamName_ + "/" + name,
                             segmentStartWallUs_);
        if (onSegmentComplete_) {
            onSegmentComplete_(sequence_, segmentPath(sequence_));
        }
    }
    deleteOldSegments();
}

void Segmenter::deleteOldSegments() {
    // event / vod 不删除历史分片
    if (playlistType_ != PlaylistType::kLive) return;
    // 仅保留窗口内的分片文件
    while (static_cast<int>(liveSegments_.size()) > windowSize_ + 1) {
        uint64_t oldSeq = liveSegments_.front();
        liveSegments_.erase(liveSegments_.begin());
        std::string path = segmentPath(oldSeq);
        if (asyncWriter_) {
            asyncWriter_->unlink(path);
        } else {
            ::unlink(path.c_str());
        }
    }
}

void Segmenter::finalize() {
    if (!started_.exchange(false)) return;
    closeCurrentSegment(lastPts_);
}

std::string Segmenter::playlist() {
    return playlist_.toString();
}

}  // namespace streaming
