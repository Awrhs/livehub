#include "webrtc/AacToOpus.hpp"

#include "core/Logger.hpp"

#include <cstring>

#ifdef HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#endif

namespace streaming {

#ifdef HAVE_LIBAV

namespace {
constexpr int kOutRate = 48000;
constexpr int kOutChannels = 2;
constexpr int kFrameSamples = 960;  // 20ms @ 48kHz

const AVCodec* findOpusEncoder() {
    if (const AVCodec* c = avcodec_find_encoder_by_name("libopus")) return c;
    return avcodec_find_encoder(AV_CODEC_ID_OPUS);  // FFmpeg 内置（实验性）
}
}  // namespace

struct AacToOpus::Impl {
    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* encFrame = nullptr;
    int64_t nextPts = -1;  // 下一帧编码输入的 48kHz 时间戳
    bool ok = false;

    ~Impl() { reset(); }

    void reset() {
        if (dec) avcodec_free_context(&dec);
        if (enc) avcodec_free_context(&enc);
        if (swr) swr_free(&swr);
        if (fifo) {
            av_audio_fifo_free(fifo);
            fifo = nullptr;
        }
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (encFrame) av_frame_free(&encFrame);
        nextPts = -1;
        ok = false;
    }
};

AacToOpus::AacToOpus() : impl_(std::make_unique<Impl>()) {}
AacToOpus::~AacToOpus() = default;

bool AacToOpus::available() { return findOpusEncoder() != nullptr; }

bool AacToOpus::ready() const { return impl_->ok; }

bool AacToOpus::init(const uint8_t* asc, size_t len) {
    Impl& m = *impl_;
    m.reset();
    const AVCodec* dc = avcodec_find_decoder(AV_CODEC_ID_AAC);
    const AVCodec* ec = findOpusEncoder();
    if (!dc || !ec) {
        LOG_WARN("AacToOpus: 缺少 AAC 解码器或 Opus 编码器");
        return false;
    }
    m.dec = avcodec_alloc_context3(dc);
    m.dec->extradata = static_cast<uint8_t*>(av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(m.dec->extradata, asc, len);
    m.dec->extradata_size = static_cast<int>(len);
    if (avcodec_open2(m.dec, dc, nullptr) < 0) {
        LOG_WARN("AacToOpus: 打开 AAC 解码器失败");
        m.reset();
        return false;
    }

    m.enc = avcodec_alloc_context3(ec);
    m.enc->sample_rate = kOutRate;
    av_channel_layout_default(&m.enc->ch_layout, kOutChannels);
    m.enc->bit_rate = 64000;
    // libopus 支持交错 float；FFmpeg 内置 opus 编码器只支持平面 float
    //（AVCodec::sample_fmts 在新版 FFmpeg 中已弃用，按编码器名选择）
    m.enc->sample_fmt = std::strcmp(ec->name, "libopus") == 0 ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
    m.enc->time_base = AVRational{1, kOutRate};
    m.enc->frame_size = kFrameSamples;
    m.enc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    av_opt_set(m.enc->priv_data, "application", "lowdelay", 0);
    if (avcodec_open2(m.enc, ec, nullptr) < 0) {
        LOG_WARN("AacToOpus: 打开 Opus 编码器失败");
        m.reset();
        return false;
    }
    if (m.enc->frame_size <= 0) m.enc->frame_size = kFrameSamples;

    m.fifo = av_audio_fifo_alloc(m.enc->sample_fmt, kOutChannels, kFrameSamples * 4);
    m.pkt = av_packet_alloc();
    m.frame = av_frame_alloc();
    m.encFrame = av_frame_alloc();
    m.ok = m.fifo && m.pkt && m.frame && m.encFrame;
    if (m.ok) LOG_INFO("AacToOpus: 初始化完成（编码器 %s）", ec->name);
    return m.ok;
}

void AacToOpus::feed(const uint8_t* data, size_t len, uint32_t timestampMs) {
    Impl& m = *impl_;
    if (!m.ok || len == 0) return;
    if (m.nextPts < 0) m.nextPts = static_cast<int64_t>(timestampMs) * (kOutRate / 1000);

    av_packet_unref(m.pkt);
    if (av_new_packet(m.pkt, static_cast<int>(len)) < 0) return;
    std::memcpy(m.pkt->data, data, len);
    if (avcodec_send_packet(m.dec, m.pkt) < 0) return;

    while (avcodec_receive_frame(m.dec, m.frame) == 0) {
        // 解码输出的格式/采样率在首帧后才确定：按需建立重采样器
        if (!m.swr) {
            if (swr_alloc_set_opts2(&m.swr, &m.enc->ch_layout, m.enc->sample_fmt, kOutRate,
                                    &m.frame->ch_layout, static_cast<AVSampleFormat>(m.frame->format),
                                    m.frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(m.swr) < 0) {
                LOG_WARN("AacToOpus: 初始化重采样失败");
                m.ok = false;
                return;
            }
        }
        int outMax = swr_get_out_samples(m.swr, m.frame->nb_samples);
        uint8_t** out = nullptr;
        if (av_samples_alloc_array_and_samples(&out, nullptr, kOutChannels, outMax, m.enc->sample_fmt, 0) < 0)
            return;
        int n = swr_convert(m.swr, out, outMax, const_cast<const uint8_t**>(m.frame->extended_data),
                            m.frame->nb_samples);
        if (n > 0) av_audio_fifo_write(m.fifo, reinterpret_cast<void**>(out), n);
        av_freep(&out[0]);
        av_freep(&out);
        av_frame_unref(m.frame);

        // 凑满 20ms 即编码一帧
        while (av_audio_fifo_size(m.fifo) >= m.enc->frame_size) {
            av_frame_unref(m.encFrame);
            m.encFrame->nb_samples = m.enc->frame_size;
            m.encFrame->format = m.enc->sample_fmt;
            m.encFrame->sample_rate = kOutRate;
            av_channel_layout_copy(&m.encFrame->ch_layout, &m.enc->ch_layout);
            if (av_frame_get_buffer(m.encFrame, 0) < 0) return;
            av_audio_fifo_read(m.fifo, reinterpret_cast<void**>(m.encFrame->data), m.enc->frame_size);
            m.encFrame->pts = m.nextPts;
            m.nextPts += m.enc->frame_size;
            if (avcodec_send_frame(m.enc, m.encFrame) < 0) continue;
            AVPacket* op = av_packet_alloc();
            while (avcodec_receive_packet(m.enc, op) == 0) {
                if (cb_) cb_(op->data, static_cast<size_t>(op->size), static_cast<uint32_t>(op->pts));
                av_packet_unref(op);
            }
            av_packet_free(&op);
        }
    }
}

#else  // !HAVE_LIBAV

struct AacToOpus::Impl {};
AacToOpus::AacToOpus() : impl_(std::make_unique<Impl>()) {}
AacToOpus::~AacToOpus() = default;
bool AacToOpus::available() { return false; }
bool AacToOpus::ready() const { return false; }
bool AacToOpus::init(const uint8_t*, size_t) { return false; }
void AacToOpus::feed(const uint8_t*, size_t, uint32_t) {}

#endif

}  // namespace streaming
