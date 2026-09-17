#pragma once

#include "core/NonCopyable.hpp"
#include "transcode/Profile.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

class TranscodeManager;
class MediaStream;

/**
 * AbrManager: 自适应码率（ABR）编排。
 *
 * 对一路源流开启 ABR 后：
 *  - 为每个档位提交转码任务，输出 RTMP 回灌为 <key>_<profile>（如 test_480p）；
 *  - 这些回灌流由现有 RtmpToHls 桥接自动生成各自的 HLS 子播放列表；
 *  - 通过 masterPlaylist() 生成 #EXT-X-STREAM-INF 主列表，播放器可自适应切换。
 *
 * 简历点：多码率转码编排、HLS master playlist、ABR。
 */
class AbrManager : NonCopyable {
public:
    AbrManager(TranscodeManager* tc, std::string rtmpOutputHost,
               std::string httpPrefix);

    /// 为源流开启 ABR（幂等）。inputUrl 为源流拉流地址（rtmp://...）。
    void enable(const std::string& streamKey,
                const std::string& inputUrl,
                const std::vector<Profile>& profiles);
    void disable(const std::string& streamKey);
    bool isEnabled(const std::string& streamKey) const;

    /// 生成主播放列表，列出各档位
    std::string masterPlaylist(const std::string& streamKey) const;

    /// 某档位对应的变体流 key（<key>_<profile>）
    static std::string variantKey(const std::string& key,
                                  const std::string& profile) {
        return key + "_" + profile;
    }

private:
    struct AbrEntry {
        std::vector<Profile> profiles;
        std::vector<std::string> taskIds;
    };

    TranscodeManager* tc_;
    std::string rtmpOutputHost_;
    std::string httpPrefix_;
    mutable std::mutex mu_;
    std::map<std::string, AbrEntry> entries_;
};

}  // namespace streaming
