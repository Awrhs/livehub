#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace streaming {

/**
 * CongestionMonitor：WHEP 下行的拥塞判断（每个观众一个）。
 *
 * 服务器只转发主播的编码码流，无法让编码器降码率；可做的是在观众链路拥塞时
 * 降低发送量——只发关键帧（相当于临时降帧率），拥塞解除后从下一个关键帧恢复完整帧率。
 *
 * 拥塞信号（任一满足即判定拥塞，并持续 kHoldMs，期间信号再次出现则顺延）：
 * - 观众的接收报告（RR）丢包率 > 10%（RFC 3550 fraction lost > 25/256）
 * - 观众的带宽估计（REMB）低于源码率的 80%
 *
 * 线程安全：源码率在发布者线程更新，RTCP 在 UDP 线程处理。
 */
class CongestionMonitor {
public:
    static constexpr int64_t kHoldMs = 3000;
    static constexpr uint8_t kLossThreshold = 25;  // 25/256 ≈ 10%

    /// 记录一个源视频/音频包（用于估计源码率，1 秒窗口）
    void onSourceBytes(size_t bytes, int64_t nowMs) {
        std::lock_guard<std::mutex> lk(mu_);
        if (windowStartMs_ == 0) windowStartMs_ = nowMs;
        windowBytes_ += bytes;
        int64_t span = nowMs - windowStartMs_;
        if (span >= 1000) {
            uint64_t bps = windowBytes_ * 8 * 1000 / static_cast<uint64_t>(span);
            sourceBps_ = sourceBps_ == 0 ? bps : (sourceBps_ * 3 + bps) / 4;
            windowBytes_ = 0;
            windowStartMs_ = nowMs;
        }
    }

    /// 观众 RR 中针对本路视频的丢包率（0-255）
    void onReceiverReport(uint8_t fractionLost, int64_t nowMs) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fractionLost > kLossThreshold) markLocked(nowMs);
    }

    /// 观众 REMB 带宽估计（bps）
    void onRemb(uint64_t bps, int64_t nowMs) {
        std::lock_guard<std::mutex> lk(mu_);
        lastRembBps_ = bps;
        if (sourceBps_ > 0 && bps * 10 < sourceBps_ * 8) markLocked(nowMs);
    }

    bool congested(int64_t nowMs) const {
        std::lock_guard<std::mutex> lk(mu_);
        return nowMs < congestedUntilMs_;
    }

    uint64_t sourceBps() const {
        std::lock_guard<std::mutex> lk(mu_);
        return sourceBps_;
    }
    uint64_t lastRembBps() const {
        std::lock_guard<std::mutex> lk(mu_);
        return lastRembBps_;
    }
    uint64_t episodes() const {
        std::lock_guard<std::mutex> lk(mu_);
        return episodes_;
    }

private:
    void markLocked(int64_t nowMs) {
        if (nowMs >= congestedUntilMs_) ++episodes_;  // 新一次拥塞
        congestedUntilMs_ = nowMs + kHoldMs;
    }

    mutable std::mutex mu_;
    int64_t windowStartMs_ = 0;
    uint64_t windowBytes_ = 0;
    uint64_t sourceBps_ = 0;
    uint64_t lastRembBps_ = 0;
    int64_t congestedUntilMs_ = 0;
    uint64_t episodes_ = 0;
};

}  // namespace streaming
