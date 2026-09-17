#include "api/StreamStats.hpp"

#include "core/Timestamp.hpp"

namespace streaming {

StreamStats::StreamStats(std::string key)
    : key_(std::move(key)),
      startedAtSec_(Timestamp::now().secondsSinceEpoch()) {}

void StreamStats::onVideoPacket(size_t bytes) {
    videoBytes_.fetch_add(static_cast<int64_t>(bytes),
                          std::memory_order_relaxed);
    videoPackets_.fetch_add(1, std::memory_order_relaxed);
}

void StreamStats::onAudioPacket(size_t bytes) {
    audioBytes_.fetch_add(static_cast<int64_t>(bytes),
                          std::memory_order_relaxed);
    audioPackets_.fetch_add(1, std::memory_order_relaxed);
}

int64_t StreamStats::startedSec() const { return startedAtSec_; }

StatsRegistry& StatsRegistry::instance() {
    static StatsRegistry reg;
    return reg;
}

std::shared_ptr<StreamStats> StatsRegistry::getOrCreate(
    const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = stats_.find(key);
    if (it != stats_.end()) return it->second;
    auto s = std::make_shared<StreamStats>(key);
    stats_[key] = s;
    return s;
}

std::shared_ptr<StreamStats> StatsRegistry::get(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = stats_.find(key);
    return it == stats_.end() ? nullptr : it->second;
}

void StatsRegistry::remove(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    stats_.erase(key);
}

std::map<std::string, std::shared_ptr<StreamStats>> StatsRegistry::all() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {stats_.begin(), stats_.end()};
}

}  // namespace streaming
