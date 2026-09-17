#pragma once

#include "core/NonCopyable.hpp"
#include "transcode/AvWrappers.hpp"
#include "transcode/Decoder.hpp"
#include "transcode/Encoder.hpp"
#include "transcode/Profile.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace streaming {

/**
 * Transcoder: 单路转码管线 Decode → (SwsScale/SwrResample) → Encode。
 *
 * 每个 Profile 对应一个 Encoder；解码后的帧经缩放/重采样后送入。
 * 编码包通过 OutputCallback 输出（送给 HlsSource / MediaStream）。
 *
 * 线程模型：start() 启动独立工作线程；stop() 通知退出并 join。
 */
class Transcoder : NonCopyable {
public:
    using OutputCallback = std::function<void(const EncodedPacket&)>;

    Transcoder(std::string inputUrl, std::vector<Profile> profiles);
    ~Transcoder();

    void setOutputCallback(OutputCallback cb) { outputCb_ = std::move(cb); }

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    const std::string& inputUrl() const { return inputUrl_; }

private:
    void run();

    std::string inputUrl_;
    std::vector<Profile> profiles_;
    OutputCallback outputCb_;

    std::unique_ptr<Decoder> decoder_;
    std::vector<std::unique_ptr<Encoder>> encoders_;
    // 每 Profile 一个 Sws / Swr
    std::vector<std::unique_ptr<SwsContextWrapper>> sws_;
    std::vector<std::unique_ptr<SwrContextWrapper>> swr_;
    std::vector<std::vector<uint8_t>> videoBuf_;
    std::vector<int> videoStrides_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
};

}  // namespace streaming
