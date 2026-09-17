#include "transcode/AvWrappers.hpp"

#ifdef HAVE_LIBAV

namespace streaming {

// ---------- AvFormatContext ----------
AvFormatContext::~AvFormatContext() { closeInput(); }

bool AvFormatContext::openInput(const std::string& url) {
    closeInput();
    return avformat_open_input(&ctx_, url.c_str(), nullptr, nullptr) == 0;
}

void AvFormatContext::closeInput() {
    if (ctx_) {
        avformat_close_input(&ctx_);
        ctx_ = nullptr;
    }
}

// ---------- AvCodecContext ----------
AvCodecContext::~AvCodecContext() { free(); }

bool AvCodecContext::alloc(const AVCodec* codec) {
    if (!codec) return false;
    if (ctx_) free();
    ctx_ = avcodec_alloc_context3(codec);
    return ctx_ != nullptr;
}

bool AvCodecContext::openCodec(AVDictionary** opts) {
    if (!ctx_ || !ctx_->codec) return false;
    return avcodec_open2(ctx_, ctx_->codec, opts) == 0;
}

bool AvCodecContext::open(const AVCodec* codec, AVDictionary** opts) {
    if (!alloc(codec)) return false;
    return openCodec(opts);
}

void AvCodecContext::free() {
    if (ctx_) {
        avcodec_free_context(&ctx_);
        ctx_ = nullptr;
    }
}

// ---------- AvPacket ----------
AvPacket::AvPacket() : pkt_(av_packet_alloc()) {}
AvPacket::~AvPacket() {
    if (pkt_) av_packet_free(&pkt_);
}
void AvPacket::unref() {
    if (pkt_) av_packet_unref(pkt_);
}

// ---------- AvFrame ----------
AvFrame::AvFrame() : frame_(av_frame_alloc()) {}
AvFrame::~AvFrame() {
    if (frame_) av_frame_free(&frame_);
}
void AvFrame::unref() {
    if (frame_) av_frame_unref(frame_);
}

// ---------- SwsContextWrapper ----------
SwsContextWrapper::~SwsContextWrapper() { free(); }

bool SwsContextWrapper::init(int srcW, int srcH, AVPixelFormat srcFmt,
                              int dstW, int dstH, AVPixelFormat dstFmt,
                              int flags) {
    free();
    ctx_ = sws_getContext(srcW, srcH, srcFmt, dstW, dstH, dstFmt, flags,
                          nullptr, nullptr, nullptr);
    return ctx_ != nullptr;
}

void SwsContextWrapper::free() {
    if (ctx_) {
        sws_freeContext(ctx_);
        ctx_ = nullptr;
    }
}

int SwsContextWrapper::scale(const uint8_t* const srcSlice[],
                              const int srcStride[], int srcSliceY,
                              int srcSliceH, uint8_t* const dst[],
                              const int dstStride[]) {
    if (!ctx_) return 0;
    return sws_scale(ctx_, srcSlice, srcStride, srcSliceY, srcSliceH, dst,
                     dstStride);
}

// ---------- SwrContextWrapper ----------
SwrContextWrapper::~SwrContextWrapper() { free(); }

bool SwrContextWrapper::init(int64_t outChLayout, AVSampleFormat outFmt,
                              int outRate, int64_t inChLayout,
                              AVSampleFormat inFmt, int inRate) {
    free();
    ctx_ = swr_alloc_set_opts(nullptr, outChLayout, outFmt, outRate,
                              inChLayout, inFmt, inRate, 0, nullptr);
    if (!ctx_) return false;
    return swr_init(ctx_) == 0;
}

void SwrContextWrapper::free() {
    if (ctx_) {
        swr_free(&ctx_);
        ctx_ = nullptr;
    }
}

int SwrContextWrapper::convert(uint8_t** out, int outCount,
                                const uint8_t** in, int inCount) {
    if (!ctx_) return 0;
    return swr_convert(ctx_, out, outCount, in, inCount);
}

}  // namespace streaming

#else

// 无 libav：stub 不需要额外定义（全部 inline 于头文件），
// 此翻译单元留空以满足构建系统的源文件收集。
namespace streaming { namespace { int av_wrappers_stub_anchor = 0; }}

#endif  // HAVE_LIBAV
