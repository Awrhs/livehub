#include "transcode/Decoder.hpp"

#include "core/Logger.hpp"

#ifdef HAVE_LIBAV
extern "C" {
#include <libavutil/time.h>
}
#endif

namespace streaming {

Decoder::Decoder() = default;
Decoder::~Decoder() { close(); }

bool Decoder::open(const std::string& url) {
    close();
#ifdef HAVE_LIBAV
    if (!fmt_.openInput(url)) {
        LOG_ERROR("Decoder: avformat_open_input failed: %s", url.c_str());
        return false;
    }
    if (avformat_find_stream_info(fmt_.get(), nullptr) < 0) {
        LOG_ERROR("Decoder: find_stream_info failed");
        return false;
    }

    for (unsigned i = 0; i < fmt_.nbStreams(); ++i) {
        AVStream* st = fmt_.get()->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            videoStreamIndex_ < 0) {
            const AVCodec* dec =
                avcodec_find_decoder(st->codecpar->codec_id);
            if (dec && videoCodec_.alloc(dec) &&
                avcodec_parameters_to_context(videoCodec_.get(),
                                              st->codecpar) >= 0 &&
                videoCodec_.openCodec()) {
                videoStreamIndex_ = static_cast<int>(i);
                videoWidth_ = videoCodec_.get()->width;
                videoHeight_ = videoCodec_.get()->height;
                videoTimeBase_ = st->time_base;
                if (st->avg_frame_rate.den != 0) {
                    videoFps_ = static_cast<int>(
                        st->avg_frame_rate.num / st->avg_frame_rate.den);
                }
            } else {
                videoCodec_.free();
            }
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                   audioStreamIndex_ < 0) {
            const AVCodec* dec =
                avcodec_find_decoder(st->codecpar->codec_id);
            if (dec && audioCodec_.alloc(dec) &&
                avcodec_parameters_to_context(audioCodec_.get(),
                                              st->codecpar) >= 0 &&
                audioCodec_.openCodec()) {
                audioStreamIndex_ = static_cast<int>(i);
                audioSampleRate_ = audioCodec_.get()->sample_rate;
                audioChannels_ = audioCodec_.get()->ch_layout.nb_channels;
                audioTimeBase_ = st->time_base;
            } else {
                audioCodec_.free();
            }
        }
    }

    opened_ = videoStreamIndex_ >= 0 || audioStreamIndex_ >= 0;
    if (!opened_) {
        LOG_ERROR("Decoder: no decodable stream in %s", url.c_str());
    }
    return opened_;
#else
    (void)url;
    LOG_WARN("Decoder: built without HAVE_LIBAV; open(%s) unavailable",
             url.c_str());
    return false;
#endif
}

void Decoder::close() {
    videoCodec_.free();
    audioCodec_.free();
    fmt_.closeInput();
    videoStreamIndex_ = -1;
    audioStreamIndex_ = -1;
    opened_ = false;
}

bool Decoder::run(const FrameCallback& cb) {
    if (!opened_) return false;
#ifdef HAVE_LIBAV
    AvPacket pkt;
    AvFrame frame;
    DecodedFrame dec;
    bool keepGoing = true;

    while (keepGoing) {
        int ret = av_read_frame(fmt_.get(), pkt.get());
        if (ret < 0) {
            // 刷新解码器
            if (videoStreamIndex_ >= 0)
                avcodec_send_packet(videoCodec_.get(), nullptr);
            if (audioStreamIndex_ >= 0)
                avcodec_send_packet(audioCodec_.get(), nullptr);
            break;
        }
        int idx = pkt->stream_index;
        AVCodecContext* cctx = nullptr;
        if (idx == videoStreamIndex_) cctx = videoCodec_.get();
        else if (idx == audioStreamIndex_) cctx = audioCodec_.get();

        if (cctx && avcodec_send_packet(cctx, pkt.get()) == 0) {
            while (avcodec_receive_frame(cctx, frame.get()) == 0) {
                dec.kind = (idx == videoStreamIndex_) ? MediaKind::kVideo
                                                       : MediaKind::kAudio;
                dec.frame = frame.get();
                dec.width = frame->width;
                dec.height = frame->height;
                dec.sampleRate = cctx->sample_rate;
                dec.channels = cctx->ch_layout.nb_channels;
                AVRational tb = (idx == videoStreamIndex_) ? videoTimeBase_
                                                            : audioTimeBase_;
                dec.ptsSeconds = (frame->pts != AV_NOPTS_VALUE && tb.den)
                    ? frame->pts * av_q2d(tb) : 0.0;
                if (!cb(dec)) {
                    keepGoing = false;
                    break;
                }
                av_frame_unref(frame.get());
            }
        }
        pkt.unref();
    }
    return true;
#else
    (void)cb;
    return false;
#endif
}

}  // namespace streaming
