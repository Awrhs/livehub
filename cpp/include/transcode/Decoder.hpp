#pragma once

#include "core/NonCopyable.hpp"
#include "transcode/AvWrappers.hpp"

#include <functional>
#include <string>

#ifdef HAVE_LIBAV
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#endif

namespace streaming {

/**
 * Decoded frame 信息（与 AVMediaType 对应）
 */
enum class MediaKind { kNone, kVideo, kAudio };

struct DecodedFrame {
    MediaKind kind = MediaKind::kNone;
#ifdef HAVE_LIBAV
    AVFrame* frame = nullptr;
#else
    void* frame = nullptr;
#endif
    // 时间基换算后的时间戳（秒）
    double ptsSeconds = 0.0;
    int width = 0;
    int height = 0;
    int sampleRate = 0;
    int channels = 0;
};

/**
 * Decoder: 打开输入（RTMP / 文件 / HTTP），解封装 + 解码。
 *
 * 流程：avformat_open_input → avformat_find_stream_info → 打开视频/音频
 *       解码器 → readPacket 循环 → 输出原始 frame。
 */
class Decoder : NonCopyable {
public:
    using FrameCallback = std::function<bool(const DecodedFrame&)>;

    Decoder();
    ~Decoder();

    bool open(const std::string& url);
    void close();
    bool isOpen() const { return opened_; }

    /// 读取并解码，每解码一帧调用 cb（返回 false 提前停止）。
    /// 返回 true 表示正常读到 EOF，false 表示出错或被回调中止。
    bool run(const FrameCallback& cb);

    int videoWidth() const { return videoWidth_; }
    int videoHeight() const { return videoHeight_; }
    int videoFps() const { return videoFps_; }
    int audioSampleRate() const { return audioSampleRate_; }
    int audioChannels() const { return audioChannels_; }

private:
    AvFormatContext fmt_;
    AvCodecContext videoCodec_;
    AvCodecContext audioCodec_;
    int videoStreamIndex_ = -1;
    int audioStreamIndex_ = -1;
    bool opened_ = false;

    int videoWidth_ = 0;
    int videoHeight_ = 0;
    int videoFps_ = 0;
    int audioSampleRate_ = 0;
    int audioChannels_ = 0;

#ifdef HAVE_LIBAV
    AVRational videoTimeBase_{0, 1};
    AVRational audioTimeBase_{0, 1};
#endif
};

}  // namespace streaming
