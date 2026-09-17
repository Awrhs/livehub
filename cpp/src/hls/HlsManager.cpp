#include "hls/HlsManager.hpp"
#include "core/AsyncFileWriter.hpp"
#include "core/Logger.hpp"

#include <vector>

namespace streaming {

HlsManager::HlsManager(std::string outputDir, int targetDuration,
                         int windowSize)
    : outputDir_(std::move(outputDir)),
      targetDuration_(targetDuration),
      windowSize_(windowSize) {}

HlsManager::~HlsManager() = default;

std::shared_ptr<HlsSource> HlsManager::getOrCreate(
    const std::string& streamKey) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sources_.find(streamKey);
    if (it != sources_.end()) return it->second;

    auto src = std::make_shared<HlsSource>(streamKey, outputDir_,
                                            targetDuration_, windowSize_);
    if (asyncWriter_) src->setAsyncWriter(asyncWriter_);
    if (!src->start()) {
        LOG_ERROR("HlsManager: failed to start source %s",
                  streamKey.c_str());
        return nullptr;
    }
    sources_[streamKey] = src;
    LOG_INFO("HlsManager: create source %s", streamKey.c_str());
    return src;
}

std::shared_ptr<HlsSource> HlsManager::find(const std::string& streamKey) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sources_.find(streamKey);
    if (it == sources_.end()) return nullptr;
    return it->second;
}

void HlsManager::remove(const std::string& streamKey) {
    std::lock_guard<std::mutex> lk(mu_);
    sources_.erase(streamKey);
}

void HlsManager::sweep(int64_t nowMs, int timeoutSec) {
    std::vector<std::string> dead;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [key, src] : sources_) {
            if (nowMs - src->lastActiveMs() >
                static_cast<int64_t>(timeoutSec) * 1000) {
                dead.push_back(key);
            }
        }
        for (auto& k : dead) {
            sources_.erase(k);
            LOG_INFO("HlsManager: sweep idle source %s", k.c_str());
        }
    }
}

size_t HlsManager::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sources_.size();
}

void HlsManager::setAsyncWriter(AsyncFileWriter* w) {
    std::lock_guard<std::mutex> lk(mu_);
    asyncWriter_ = w;
    for (auto& [key, src] : sources_) {
        (void)key;
        src->setAsyncWriter(w);
    }
}

}  // namespace streaming
