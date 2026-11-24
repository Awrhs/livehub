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

namespace {
std::string segName(uint64_t seq) {
    char name[32];
    std::snprintf(name, sizeof(name), "%llu.ts",
                  static_cast<unsigned long long>(seq));
    return name;
}
}  // namespace

Segmenter::Segmenter(std::string outputDir, std::string streamName,
                     int targetDuration, int windowSize)
    : outputDir_(std::move(outputDir)),
      streamName_(std::move(streamName)),
      targetDuration_(targetDuration),
      windowSize_(windowSize),
      st_(std::make_shared<State>(targetDuration, windowSize)) {
    muxer_ = std::make_unique<TsMuxer>(
        [this](const uint8_t* d, size_t n) { this->writeTs(d, n); });
}

Segmenter::~Segmenter() {
    if (started_.exchange(false)) closeCurrentSegment(lastPts_, true);
    // live 模式：删除残留分片；之后才完成写盘的分片在回调中直接删除
    std::vector<uint64_t> files;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        if (st_->type != PlaylistType::kLive) return;
        st_->cleanup = true;
        files.swap(st_->onDisk);
    }
    for (uint64_t seq : files) {
        std::string path = segmentPath(seq);
        if (asyncWriter_) asyncWriter_->unlink(path);
        else ::unlink(path.c_str());
    }
}

void Segmenter::setPlaylistType(PlaylistType t) {
    std::lock_guard<std::mutex> lk(st_->mu);
    st_->playlist.setType(t);
    st_->type = t;
}

void Segmenter::setSegmentCompleteCallback(SegmentCompleteCallback cb) {
    std::lock_guard<std::mutex> lk(st_->mu);
    st_->onComplete = std::move(cb);
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
    newSegment(-1, false);  // 起始时间戳由首帧确定
    started_ = true;
    return true;
}

std::string Segmenter::segmentPath(uint64_t sequence) const {
    return outputDir_ + "/" + streamName_ + "/" + segName(sequence);
}

void Segmenter::noteTimestamp(int64_t ts90k) {
    if (ts90k < 0) return;
    if (segmentStartPts_ < 0) segmentStartPts_ = ts90k;
    if (ts90k > lastPts_) lastPts_ = ts90k;
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
    if (segmentOpen_) closeCurrentSegment(boundaryPts, false);

    if (discontinuity) {
        std::lock_guard<std::mutex> lk(st_->mu);
        st_->playlist.setDiscontinuity();
    }

    ++sequence_;
    std::string path = segmentPath(sequence_);
    if (asyncWriter_) {
        currentAsyncId_ = asyncWriter_->open(path, /*append=*/false, /*discardIfIncomplete=*/true);
        segmentOpen_ = currentAsyncId_ != 0;
    } else {
        currentFile_.open(path, std::ios::binary | std::ios::trunc);
        segmentOpen_ = currentFile_.is_open();
        if (!segmentOpen_) {
            LOG_ERROR("Segmenter open %s failed: %s", path.c_str(),
                      strerror(errno));
            return;
        }
    }
    segmentStartPts_ = boundaryPts;
    lastPts_ = boundaryPts;
    segmentStartWallUs_ = Timestamp::now().microSecondsSinceEpoch();

    LOG_DEBUG("Segmenter [%s] new segment %llu", streamName_.c_str(),
              static_cast<unsigned long long>(sequence_));
}

void Segmenter::publishSegment(const std::shared_ptr<State>& st,
                               const std::string& dir, uint64_t seq,
                               double duration, int64_t wallUs, bool valid,
                               bool endList, const std::string& streamName, bool damaged) {
    std::vector<uint64_t> toDelete;
    SegmentCompleteCallback cb;
    {
        std::lock_guard<std::mutex> lk(st->mu);
        if (st->cleanup || !valid) {
            toDelete.push_back(seq);  // 已析构或空分片：直接删除
            // 分片因写盘不完整被丢弃：时间线出现缺口，下一分片标记 discontinuity
            if (damaged && !st->cleanup) st->playlist.setDiscontinuity();
        } else {
            // URI 相对 /live/<stream>.m3u8：<stream>/<seq>.ts
            st->playlist.addSegment(seq, duration, streamName + "/" + segName(seq),
                                    wallUs);
            st->onDisk.push_back(seq);
            if (st->type == PlaylistType::kLive) {
                while (static_cast<int>(st->onDisk.size()) > st->keepFiles) {
                    toDelete.push_back(st->onDisk.front());
                    st->onDisk.erase(st->onDisk.begin());
                }
            }
            cb = st->onComplete;
        }
        if (endList) {
            st->playlist.endList();
            st->ended = true;
        }
    }
    for (uint64_t s : toDelete) {
        ::unlink((dir + "/" + segName(s)).c_str());
    }
    if (cb && valid) cb(seq, dir + "/" + segName(seq));
}

void Segmenter::closeCurrentSegment(int64_t endPts, bool endList) {
    bool wasOpen = segmentOpen_;
    segmentOpen_ = false;
    bool valid = wasOpen && segmentStartPts_ >= 0 && endPts > segmentStartPts_;
    double duration =
        valid ? static_cast<double>(endPts - segmentStartPts_) / 90000.0 : 0.0;
    uint64_t seq = sequence_;
    int64_t wallUs = segmentStartWallUs_;
    std::string dir = outputDir_ + "/" + streamName_;
    auto st = st_;
    std::string name = streamName_;

    if (asyncWriter_ && currentAsyncId_ != 0) {
        uint64_t id = currentAsyncId_;
        currentAsyncId_ = 0;
        // 写盘线程完成 close 后才发布：保证播放器拿到的分片是完整文件
        asyncWriter_->close(id, [st, dir, seq, duration, wallUs, valid, endList,
                                 name](bool ok, uint64_t) {
            // 写入不完整的分片不进入播放列表
            publishSegment(st, dir, seq, duration, wallUs, valid && ok, endList, name, valid && !ok);
        });
        return;
    }
    if (currentFile_.is_open()) currentFile_.close();
    if (wasOpen || endList)
        publishSegment(st, dir, seq, duration, wallUs, valid && wasOpen,
                       endList, name);
}

void Segmenter::endOfStream() {
    if (!started_.exchange(false)) return;
    closeCurrentSegment(lastPts_, true);
}

void Segmenter::resume() {
    if (started_) return;
    {
        std::lock_guard<std::mutex> lk(st_->mu);
        st_->playlist.resume();
        st_->playlist.setDiscontinuity();
        st_->ended = false;
    }
    newSegment(-1, false);
    started_ = true;
}

bool Segmenter::ended() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->ended;
}

std::string Segmenter::playlist() {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->playlist.toString();
}

}  // namespace streaming
