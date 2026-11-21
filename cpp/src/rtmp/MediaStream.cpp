#include "rtmp/MediaStream.hpp"

#include <algorithm>

namespace streaming {

MediaStream::MediaStream(std::string streamKey, std::string app)
    : streamKey_(std::move(streamKey)), app_(std::move(app)) {}

void MediaStream::onPacket(MediaPacket pkt) {
    std::lock_guard<std::mutex> dlk(dispatchMu_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pkt.isSequenceHeader) {
            // seq header 由 set*SequenceHeader 单独保存、回放时最先发送，不进 GOP
        } else if (pkt.type == MediaPacket::kVideo) {
            if (pkt.isKeyframe) {
                gop_.clear();
                gopHasKeyframe_ = true;
                gop_.push_back(pkt);
            } else if (gopHasKeyframe_) {
                gop_.push_back(pkt);
            }
        } else if (pkt.type == MediaPacket::kAudio) {
            // 仅在已有关键帧时缓存音频（纯音频流不缓存，避免无界增长）
            if (gopHasKeyframe_) gop_.push_back(pkt);
        }
        if (gop_.size() > gopMaxPackets_) {
            // GOP 过长（关键帧间隔异常）：整体丢弃，不能只删头部（会删掉 IDR）
            gop_.clear();
            gopHasKeyframe_ = false;
        }
    }

    // 通知订阅者（拷贝回调列表，回调中可安全 unsubscribe）
    std::vector<PacketCallback> cbs;
    {
        std::lock_guard<std::mutex> lk(subMu_);
        cbs.reserve(subscribers_.size());
        for (auto& [id, cb] : subscribers_) cbs.push_back(cb);
    }
    for (auto& cb : cbs) {
        if (cb) cb(pkt);
    }
}

MediaStream::SubscriberId MediaStream::subscribeWithGop(PacketCallback cb) {
    std::lock_guard<std::mutex> dlk(dispatchMu_);
    playGop(cb);
    return subscribe(std::move(cb));
}

MediaStream::SubscriberId MediaStream::subscribe(PacketCallback cb) {
    std::lock_guard<std::mutex> lk(subMu_);
    SubscriberId id = nextSubId_++;
    subscribers_.emplace_back(id, std::move(cb));
    return id;
}

void MediaStream::unsubscribe(SubscriberId id) {
    std::lock_guard<std::mutex> lk(subMu_);
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
                        [id](const auto& p) { return p.first == id; }),
        subscribers_.end());
}

void MediaStream::playGop(const PacketCallback& cb) const {
    std::lock_guard<std::mutex> lk(mu_);
    // 先推 sequence header，让新观众能解码
    if (!videoSeqHeader_.empty()) {
        MediaPacket sh;
        sh.type = MediaPacket::kVideo;
        sh.timestamp = 0;
        sh.data = videoSeqHeader_;
        sh.isKeyframe = true;
        sh.isSequenceHeader = true;
        cb(sh);
    }
    if (!audioSeqHeader_.empty()) {
        MediaPacket sh;
        sh.type = MediaPacket::kAudio;
        sh.timestamp = 0;
        sh.data = audioSeqHeader_;
        sh.isSequenceHeader = true;
        cb(sh);
    }
    for (const auto& pkt : gop_) {
        cb(pkt);
    }
}

uint32_t MediaStream::subscriberCount() const {
    std::lock_guard<std::mutex> lk(subMu_);
    return static_cast<uint32_t>(subscribers_.size());
}

}  // namespace streaming
