#include "transcode/Transcoder.hpp"

#include "core/Logger.hpp"

#ifdef HAVE_LIBAV
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
}
#endif

namespace streaming {

Transcoder::Transcoder(std::string inputUrl, std::vector<Profile> profiles)
    : inputUrl_(std::move(inputUrl)), profiles_(std::move(profiles)) {}

Transcoder::~Transcoder() {
    stop();
}

bool Transcoder::start() {
    if (running_.exchange(true)) return false;
    stop_.store(false);
    worker_ = std::thread([this] { run(); });
    return true;
}

void Transcoder::stop() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

void Transcoder::run() {
#ifdef HAVE_LIBAV
    decoder_ = std::make_unique<Decoder>();
    if (!decoder_->open(inputUrl_)) {
        LOG_ERROR("Transcoder: cannot open input %s", inputUrl_.c_str());
        running_.store(false);
        return;
    }

    // 为每个 profile 创建编码器
    for (const auto& p : profiles_) {
        auto enc = std::make_unique<Encoder>(p);
        bool vok = enc->openVideo(decoder_->videoWidth(),
                                   decoder_->videoHeight(),
                                   decoder_->videoFps());
        bool aok = enc->openAudio(decoder_->audioSampleRate() > 0
                                       ? decoder_->audioSampleRate() : 44100,
                                   decoder_->audioChannels() > 0
                                       ? decoder_->audioChannels() : 2);
        if (!vok && !aok) {
            LOG_ERROR("Transcoder: encoder %s failed to open", p.name.c_str());
            continue;
        }

        // 视频缩放上下文
        auto sws = std::make_unique<SwsContextWrapper>();
        if (vok) {
            int dstW = p.width > 0 ? p.width : decoder_->videoWidth();
            int dstH = p.height > 0 ? p.height : decoder_->videoHeight();
            sws->init(decoder_->videoWidth(), decoder_->videoHeight(),
                      AV_PIX_FMT_YUV420P, dstW, dstH,
                      AV_PIX_FMT_YUV420P, SWS_BICUBIC);
        }
        // 音频重采样上下文（简化：直通同样格式时不建）
        auto swr = std::make_unique<SwrContextWrapper>();

        encoders_.push_back(std::move(enc));
        sws_.push_back(std::move(sws));
        swr_.push_back(std::move(swr));
    }

    if (encoders_.empty()) {
        LOG_ERROR("Transcoder: no encoder available");
        running_.store(false);
        return;
    }

    // 解码循环
    decoder_->run([this](const DecodedFrame& f) {
        if (stop_.load()) return false;
#ifdef HAVE_LIBAV
        AVFrame* frame = static_cast<AVFrame*>(f.frame);
        if (!frame) return true;

        if (f.kind == MediaKind::kVideo) {
            for (size_t i = 0; i < encoders_.size(); ++i) {
                const Profile& p = encoders_[i]->profile();
                int dstW = p.width > 0 ? p.width : frame->width;
                int dstH = p.height > 0 ? p.height : frame->height;
                // 分配输出缓冲
                int bufSize = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
                                                        dstW, dstH, 1);
                std::vector<uint8_t> outBuf(bufSize);
                uint8_t* dst[4] = {nullptr};
                int dstStride[4] = {0};
                av_image_fill_arrays(dst, dstStride, outBuf.data(),
                                     AV_PIX_FMT_YUV420P, dstW, dstH, 1);
                sws_[i]->scale(frame->data, frame->linesize, 0, frame->height,
                               dst, dstStride);
                const uint8_t* planes[4] = {dst[0], dst[1], dst[2], dst[3]};
                int strides[4] = {dstStride[0], dstStride[1], dstStride[2],
                                   dstStride[3]};
                encoders_[i]->sendVideoFrame(
                    planes, strides, frame->pts,
                    [this](const EncodedPacket& ep) {
                        if (outputCb_) outputCb_(ep);
                    });
            }
        } else if (f.kind == MediaKind::kAudio) {
            for (auto& enc : encoders_) {
                if (enc->isAudioOpen()) {
                    enc->sendAudioFrame(
                        const_cast<const uint8_t* const*>(frame->data),
                        frame->nb_samples, frame->pts,
                        [this](const EncodedPacket& ep) {
                            if (outputCb_) outputCb_(ep);
                        });
                }
            }
        }
#endif
        return !stop_.load();
    });

    // flush
    for (auto& enc : encoders_) {
        enc->flushVideo([this](const EncodedPacket& ep) {
            if (outputCb_) outputCb_(ep);
        });
        enc->flushAudio([this](const EncodedPacket& ep) {
            if (outputCb_) outputCb_(ep);
        });
    }

    running_.store(false);
    LOG_INFO("Transcoder: finished %s", inputUrl_.c_str());
#else
    LOG_WARN("Transcoder: built without HAVE_LIBAV");
    running_.store(false);
#endif
}

}  // namespace streaming
