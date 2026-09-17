#include "hls/AbrManager.hpp"

#include "core/Logger.hpp"
#include "hls/M3u8Playlist.hpp"
#include "transcode/TranscodeManager.hpp"

#include <sstream>

namespace streaming {

AbrManager::AbrManager(TranscodeManager* tc, std::string rtmpOutputHost,
                       std::string httpPrefix)
    : tc_(tc),
      rtmpOutputHost_(std::move(rtmpOutputHost)),
      httpPrefix_(std::move(httpPrefix)) {}

void AbrManager::enable(const std::string& streamKey,
                        const std::string& inputUrl,
                        const std::vector<Profile>& profiles) {
    if (!tc_) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (entries_.count(streamKey)) return;  // 已开启

    AbrEntry entry;
    entry.profiles = profiles;

    // 为每个档位提交一个转码任务，输出回灌到 <key>_<profile>
    for (const auto& p : profiles) {
        std::string outPrefix =
            rtmpOutputHost_ + "/live/" + variantKey(streamKey, p.name);
        std::string id = tc_->submit(inputUrl, outPrefix, {p}, true);
        entry.taskIds.push_back(id);
        LOG_INFO("ABR: submitted %s for %s task=%s out=%s",
                 p.name.c_str(), streamKey.c_str(), id.c_str(),
                 outPrefix.c_str());
    }
    entries_[streamKey] = std::move(entry);
}

void AbrManager::disable(const std::string& streamKey) {
    if (!tc_) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(streamKey);
    if (it == entries_.end()) return;
    for (const auto& id : it->second.taskIds) tc_->cancel(id);
    entries_.erase(it);
}

bool AbrManager::isEnabled(const std::string& streamKey) const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.count(streamKey) > 0;
}

std::string AbrManager::masterPlaylist(const std::string& streamKey) const {
    std::vector<std::pair<std::string, int>> variants;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(streamKey);
    if (it != entries_.end()) {
        for (const auto& p : it->second.profiles) {
            // 子播放列表走现有 HLS 路由 /live/<variant>.m3u8
            int bandwidth = (p.videoBitrate + p.audioBitrate);
            if (bandwidth == 0) bandwidth = 1'000'000;
            variants.emplace_back(
                httpPrefix_ + "/live/" + variantKey(streamKey, p.name) +
                    ".m3u8",
                bandwidth);
        }
    } else {
        // 未开 ABR：仅原始流
        variants.emplace_back(httpPrefix_ + "/live/" + streamKey + ".m3u8",
                              2'500'000);
    }
    return M3u8Playlist::buildMasterPlaylist(variants);
}

}  // namespace streaming
