#include "transcode/Encoder.hpp"

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

Encoder::Encoder(Profile profile) : profile_(std::move(profile)) {}
Encoder::~Encoder() { close(); }

bool Encoder::openVideo(int srcWidth, int srcHeight, int srcFps) {
    close();
#ifdef HAVE_LIBAV
    const AVCodec* codec = avcodec_find_encoder_by_name(
        profile_.videoCodec.c_str());
    if (!codec) {
        LOG_ERROR("Encoder: video codec not found: %s",
                  profile_.videoCodec.c_str());
        return false;
    }
    if (!video_.alloc(codec)) return false;
    AVCodecContext* c = video_.get();

    int w = profile_.width > 0 ? profile_.width : srcWidth;
    int h = profile_.height > 0 ? profile_.height : srcHeight;
    c->width = w;
    c->height = h;
    c->time_base = AVRational{1, 90000};
    c->framerate = AVRational{
        profile_.fps > 0 ? profile_.fps : (srcFps > 0 ? srcFps : 30), 1};
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->bit_rate = profile_.videoBitrate > 0 ? profile_.videoBitrate : 2'000'000;
    c->gop_size = profile_.gopSize > 0 ? profile_.gopSize : 60;
    c->max_b_frames = 0;  // zerolatency

    av_opt_set(c->priv_data, "preset", profile_.preset.c_str(), 0);
    if (!profile_.tune.empty()) {
        av_opt_set(c->priv_data, "tune", profile_.tune.c_str(), 0);
    }

    if (video_.openCodec()) {
        videoOpen_ = true;
        return true;
    }
    LOG_ERROR("Encoder: video openCodec failed");
    return false;
#else
    (void)srcWidth; (void)srcHeight; (void)srcFps;
    return false;
#endif
}

bool Encoder::openAudio(int sampleRate, int channels) {
#ifdef HAVE_LIBAV
    const AVCodec* codec =
        avcodec_find_encoder_by_name(profile_.audioCodec.c_str());
    if (!codec) return false;
    if (!audio_.alloc(codec)) return false;
    AVCodecContext* c = audio_.get();
    c->sample_rate = sampleRate;
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->bit_rate = profile_.audioBitrate > 0 ? profile_.audioBitrate : 128'000;
    av_channel_layout_default(&c->ch_layout, channels);
    if (audio_.openCodec()) {
        audioOpen_ = true;
        return true;
    }
    return false;
#else
    (void)sampleRate; (void)channels;
    return false;
#endif
}

void Encoder::close() {
    video_.free();
    audio_.free();
    videoOpen_ = false;
    audioOpen_ = false;
}

#ifdef HAVE_LIBAV
bool Encoder::drainVideo(AVCodecContext* ctx, const PacketCallback& cb) {
    AvPacket pkt;
    while (avcodec_receive_packet(ctx, pkt.get()) == 0) {
        EncodedPacket ep;
        ep.isVideo = true;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts = pkt->pts;
        ep.dts = pkt->dts;
        ep.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        cb(ep);
        pkt.unref();
    }
    return true;
}

bool Encoder::drainAudio(AVCodecContext* ctx, const PacketCallback& cb) {
    AvPacket pkt;
    while (avcodec_receive_packet(ctx, pkt.get()) == 0) {
        EncodedPacket ep;
        ep.isVideo = false;
        ep.data.assign(pkt->data, pkt->data + pkt->size);
        ep.pts = pkt->pts;
        ep.dts = pkt->dts;
        cb(ep);
        pkt.unref();
    }
    return true;
}
#endif

bool Encoder::sendVideoFrame(const uint8_t* const* planes, const int* strides,
                              int64_t pts, const PacketCallback& cb) {
#ifdef HAVE_LIBAV
    if (!videoOpen_) return false;
    AVCodecContext* c = video_.get();
    AVFrame* f = videoFrame_.get();
    f->format = c->pix_fmt;
    f->width = c->width;
    f->height = c->height;
    f->pts = pts;
    av_frame_unref(f);
    av_image_fill_arrays(f->data, f->linesize, nullptr, c->pix_fmt,
                         c->width, c->height, 1);
    for (int i = 0; i < 4 && planes[i]; ++i) {
        f->data[i] = const_cast<uint8_t*>(planes[i]);
        f->linesize[i] = strides[i];
    }
    if (avcodec_send_frame(c, f) == 0) {
        drainVideo(c, cb);
    }
    return true;
#else
    (void)planes; (void)strides; (void)pts; (void)cb;
    return false;
#endif
}

bool Encoder::sendAudioFrame(const uint8_t* const* planes, int nbSamples,
                              int64_t pts, const PacketCallback& cb) {
#ifdef HAVE_LIBAV
    if (!audioOpen_) return false;
    AVCodecContext* c = audio_.get();
    AVFrame* f = audioFrame_.get();
    av_frame_unref(f);
    f->format = c->sample_fmt;
    f->nb_samples = nbSamples;
    av_channel_layout_copy(&f->ch_layout, &c->ch_layout);
    f->sample_rate = c->sample_rate;
    f->pts = pts;
    for (int i = 0; i < 8 && planes[i]; ++i) {
        f->data[i] = const_cast<uint8_t*>(planes[i]);
    }
    if (avcodec_send_frame(c, f) == 0) {
        drainAudio(c, cb);
    }
    return true;
#else
    (void)planes; (void)nbSamples; (void)pts; (void)cb;
    return false;
#endif
}

void Encoder::flushVideo(const PacketCallback& cb) {
#ifdef HAVE_LIBAV
    if (videoOpen_) {
        avcodec_send_frame(video_.get(), nullptr);
        drainVideo(video_.get(), cb);
    }
#else
    (void)cb;
#endif
}

void Encoder::flushAudio(const PacketCallback& cb) {
#ifdef HAVE_LIBAV
    if (audioOpen_) {
        avcodec_send_frame(audio_.get(), nullptr);
        drainAudio(audio_.get(), cb);
    }
#else
    (void)cb;
#endif
}

}  // namespace streaming
