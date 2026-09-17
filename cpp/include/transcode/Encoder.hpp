#pragma once

#include "core/NonCopyable.hpp"
#include "transcode/AvWrappers.hpp"
#include "transcode/Profile.hpp"

#include <functional>

#ifdef HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

namespace streaming {

/**
 * EncodedPacket: 编码后输出（H.264/AAC）。
 */
struct EncodedPacket {
    bool isVideo = true;
    std::vector<uint8_t> data;
    int64_t pts = 0;
    int64_t dts = 0;
    bool keyframe = false;
};

/**
 * Encoder: 按 Profile 配置视频/音频编码器。
 *
 * 使用 send_frame / receive_packet 新 API，输出回调逐包送出。
 */
class Encoder : NonCopyable {
public:
    using PacketCallback = std::function<void(const EncodedPacket&)>;

    explicit Encoder(Profile profile);
    ~Encoder();

    bool openVideo(int srcWidth, int srcHeight, int srcFps);
    bool openAudio(int sampleRate, int channels);
    void close();

    bool isVideoOpen() const { return videoOpen_; }
    bool isAudioOpen() const { return audioOpen_; }

    /// 送一帧原始视频（AV_PIX_FMT_YUV420P）并刷出编码包
    bool sendVideoFrame(const uint8_t* const* planes, const int* strides,
                        int64_t pts, const PacketCallback& cb);
    /// 送一帧原始音频并刷出编码包
    bool sendAudioFrame(const uint8_t* const* planes, int nbSamples,
                        int64_t pts, const PacketCallback& cb);

    /// 刷新编码器缓冲
    void flushVideo(const PacketCallback& cb);
    void flushAudio(const PacketCallback& cb);

    const Profile& profile() const { return profile_; }

private:
#ifdef HAVE_LIBAV
    bool drainVideo(AVCodecContext* ctx, const PacketCallback& cb);
    bool drainAudio(AVCodecContext* ctx, const PacketCallback& cb);
#endif

    Profile profile_;
    AvCodecContext video_;
    AvCodecContext audio_;
    bool videoOpen_ = false;
    bool audioOpen_ = false;

    AvFrame videoFrame_;
    AvFrame audioFrame_;
};

}  // namespace streaming
