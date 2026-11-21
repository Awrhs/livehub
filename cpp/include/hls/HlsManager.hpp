#pragma once

#include "core/NonCopyable.hpp"
#include "hls/HlsSource.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace streaming {

class AsyncFileWriter;
class EventLoop;

/**
 * HlsManager: HLS 源流注册表。
 *
 * 按 streamKey 管理 HlsSource，提供创建/查找/过期回收。
 * 线程安全：内部加锁保护注册表。
 */
class HlsManager : NonCopyable {
public:
    explicit HlsManager(std::string outputDir = "./hls",
                         int targetDuration = 6, int windowSize = 5);
    ~HlsManager();

    /// 获取或创建一路 HLS 源流
    std::shared_ptr<HlsSource> getOrCreate(const std::string& streamKey);

    /// 查找已存在的源流（不存在返回 nullptr）
    std::shared_ptr<HlsSource> find(const std::string& streamKey);

    /// 移除一路流
    void remove(const std::string& streamKey);

    /// 推流结束：m3u8 追加 ENDLIST；源保留到 sweep 超时后清理（届时删除分片文件）
    void endStream(const std::string& streamKey);
    std::vector<std::string> keys() const;

    /// 周期回收过期流（无活跃超过 timeoutSec 秒），在 EventLoop 定时器中调用
    void sweep(int64_t nowMs, int timeoutSec);

    /// 为所有（含未来新建的）HlsSource 设置异步文件写线程
    void setAsyncWriter(class AsyncFileWriter* w);

    const std::string& outputDir() const { return outputDir_; }

    size_t size() const;

private:
    std::string outputDir_;
    int targetDuration_;
    int windowSize_;
    AsyncFileWriter* asyncWriter_ = nullptr;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<HlsSource>> sources_;
};

}  // namespace streaming
