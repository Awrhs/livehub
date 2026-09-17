#pragma once

#include "core/NonCopyable.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#endif

namespace streaming {

/**
 * AvWrappers: FFmpeg libav 的 RAII 包装。
 *
 * 全部类在编译时根据 HAVE_LIBAV 切换：有 libav 时持有真实句柄，
 * 无 libav 时提供空实现（stub），保证无依赖环境可编译。
 *
 * 简历点：RAII 资源管理、转码管线（解码→缩放/重采样→编码）。
 */

#ifdef HAVE_LIBAV

// ---------- 真实实现 ----------

class AvFormatContext : NonCopyable {
public:
    AvFormatContext() = default;
    ~AvFormatContext();

    AVFormatContext* get() const { return ctx_; }
    AVFormatContext** operator&() { return &ctx_; }
    bool openInput(const std::string& url);
    void closeInput();
    AVStream** streams() { return ctx_ ? ctx_->streams : nullptr; }
    unsigned nbStreams() const { return ctx_ ? ctx_->nb_streams : 0; }

private:
    AVFormatContext* ctx_ = nullptr;
};

class AvCodecContext : NonCopyable {
public:
    AvCodecContext() = default;
    ~AvCodecContext();

    AVCodecContext* get() const { return ctx_; }
    AVCodecContext** operator&() { return &ctx_; }
    bool alloc(const AVCodec* codec);
    bool openCodec(AVDictionary** opts = nullptr);
    bool open(const AVCodec* codec, AVDictionary** opts = nullptr);
    void free();

private:
    AVCodecContext* ctx_ = nullptr;
};

class AvPacket : NonCopyable {
public:
    AvPacket();
    ~AvPacket();
    AVPacket* get() const { return pkt_; }
    AVPacket* operator->() const { return pkt_; }
    void unref();

private:
    AVPacket* pkt_ = nullptr;
};

class AvFrame : NonCopyable {
public:
    AvFrame();
    ~AvFrame();
    AVFrame* get() const { return frame_; }
    AVFrame* operator->() const { return frame_; }
    void unref();

private:
    AVFrame* frame_ = nullptr;
};

class SwsContextWrapper : NonCopyable {
public:
    SwsContextWrapper() = default;
    ~SwsContextWrapper();
    bool init(int srcW, int srcH, AVPixelFormat srcFmt, int dstW, int dstH,
              AVPixelFormat dstFmt, int flags = SWS_BICUBIC);
    void free();
    int scale(const uint8_t* const srcSlice[], const int srcStride[],
              int srcSliceY, int srcSliceH, uint8_t* const dst[],
              const int dstStride[]);
    SwsContext* get() const { return ctx_; }

private:
    SwsContext* ctx_ = nullptr;
};

class SwrContextWrapper : NonCopyable {
public:
    SwrContextWrapper() = default;
    ~SwrContextWrapper();
    bool init(int64_t outChLayout, AVSampleFormat outFmt, int outRate,
              int64_t inChLayout, AVSampleFormat inFmt, int inRate);
    void free();
    int convert(uint8_t** out, int outCount, const uint8_t** in, int inCount);
    SwrContext* get() const { return ctx_; }

private:
    SwrContext* ctx_ = nullptr;
};

#else  // !HAVE_LIBAV

// ---------- Stub（无 libav 环境） ----------
// 提供最小空壳，使调用方可编译，运行时返回失败。

struct AVFormatContextStub {
    void* streams;
    unsigned nb_streams = 0;
};
struct AVCodecContextStub {};
struct AVPacketStub {};
struct AVFrameStub {};
struct SwsContextStub {};
struct SwrContextStub {};

class AvFormatContext : NonCopyable {
public:
    bool openInput(const std::string&) { return false; }
    void closeInput() {}
    unsigned nbStreams() const { return 0; }
    AVFormatContextStub* get() const { return nullptr; }
};
class AvCodecContext : NonCopyable {
public:
    bool alloc(void*) { return false; }
    bool openCodec(void* = nullptr) { return false; }
    bool open(void*, void* = nullptr) { return false; }
    void free() {}
    AVCodecContextStub* get() const { return nullptr; }
};
class AvPacket : NonCopyable {
public:
    AvPacket() {}
    void unref() {}
    AVPacketStub* get() const { return nullptr; }
};
class AvFrame : NonCopyable {
public:
    AvFrame() {}
    void unref() {}
    AVFrameStub* get() const { return nullptr; }
};
class SwsContextWrapper : NonCopyable {
public:
    bool init(int, int, int, int, int, int, int = 4) { return false; }
    void free() {}
    int scale(const uint8_t* const[], const int[], int, int, uint8_t* const[],
              const int[]) { return 0; }
    SwsContextStub* get() const { return nullptr; }
};
class SwrContextWrapper : NonCopyable {
public:
    bool init(int64_t, int, int, int64_t, int, int) { return false; }
    void free() {}
    int convert(uint8_t**, int, const uint8_t**, int) { return 0; }
    SwrContextStub* get() const { return nullptr; }
};

#endif  // HAVE_LIBAV

}  // namespace streaming
