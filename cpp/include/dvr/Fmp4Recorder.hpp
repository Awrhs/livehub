#pragma once

#include "core/NonCopyable.hpp"
#include "dvr/Mp4BoxWriter.hpp"
#include "rtmp/MediaStream.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

class AsyncFileWriter;

/**
 * Fmp4Recorder: 把一路 MediaStream 录制为 fragmented MP4。
 *
 * 两种用法：
 *  1. timeshift / catch-up：按 segmentDuration 切分，写入 segments/<seg>.m4s，
 *     init.mp4 只写一次；保留最近 windowCount 个分片供时移回看。
 *  2. live 录像（VOD）：所有 moof/mdat 追加到同一个文件，结束后可直接播放。
 *
 * 解析 RTMP 音视频 tag：
 *  - video: avcType 0 = AVCDecoderConfigurationRecord（avcC）；
 *           avcType 1 = 一个 sample（AVCC 长度前缀 NALU，可直接入 mdat）。
 *  - audio: AAC sequence header = AudioSpecificConfig；AAC raw = 一个 sample。
 * 时间戳：RTMP 为 ms；video timescale 90000，audio timescale 为采样率。
 *
 * 简历点：手写 ISO-BMFF/fMP4、SPS 解析、GOP 对齐切片、时移窗口清理。
 */
class Fmp4Recorder : NonCopyable,
                     public std::enable_shared_from_this<Fmp4Recorder> {
public:
    using SegmentCallback =
        std::function<void(uint32_t seq, const std::string& path)>;

    struct Options {
        std::string outputDir;        // 分片目录（时移）或文件路径（VOD）
        uint32_t segmentDurationMs = 6000;
        uint32_t windowCount = 12;     // 时移保留分片数（0 = 不清理）
        bool vodMode = false;          // true = 单文件录制
        uint16_t width = 1280;
        uint16_t height = 720;
        uint16_t audioSampleRate = 44100;
        uint8_t audioChannels = 2;
    };

    Fmp4Recorder(std::shared_ptr<MediaStream> stream, Options opt);
    ~Fmp4Recorder();

    void start();
    void stop();

    /// 注入异步文件写线程（可选）。设置后分片写盘不阻塞调用线程。
    void setAsyncWriter(AsyncFileWriter* w) { asyncWriter_ = w; }

    /// 时移：返回 init.mp4 与分片路径列表（按序号排序）
    std::vector<std::string> playlist() const;
    std::string initPath() const { return opt_.outputDir + "/init.mp4"; }
    uint32_t currentSeq() const { return segmentSeq_; }

    void setSegmentCallback(SegmentCallback cb) { onSegment_ = std::move(cb); }

private:
    void onPacket(const MediaPacket& pkt);
    void handleVideo(const MediaPacket& pkt);
    void handleAudio(const MediaPacket& pkt);
    void ensureInit();
    void flushFragment(bool force);
    void writeFile(const std::string& path, const std::vector<uint8_t>& data);
    void appendFile(const std::string& path, const std::vector<uint8_t>& data);
    void pruneOldSegments();
    void parseSpsDimensions();

    std::shared_ptr<MediaStream> stream_;
    Options opt_;
    MediaStream::SubscriberId subId_ = 0;

    std::atomic<bool> running_{false};
    mutable std::mutex mu_;

    bool inited_ = false;
    bool isHevc_ = false;
    std::vector<uint8_t> avcc_;   // AVC AVCDecoderConfigurationRecord 或 HEVC hvCC
    std::vector<uint8_t> asc_;
    bool hasAudio_ = false;

    // video timescale 90000
    static constexpr uint32_t kVideoTimescale = 90000;
    static constexpr uint32_t kAudioTimescaleDefault = 44100;
    uint32_t audioTimescale_ = kAudioTimescaleDefault;

    // 分片状态
    uint32_t segmentSeq_ = 1;    // 段序号（文件名 seg-N.m4s）
    uint32_t fragSeq_ = 1;       // moof 全局序号（mfhd.sequence_number，单调递增）
    uint64_t videoBaseDts_ = 0;   // 当前分片起始 dts（90k）
    uint64_t audioBaseDts_ = 0;
    uint32_t videoFirstDts_ = 0;  // 首帧 dts，作全局基准
    uint32_t audioFirstDts_ = 0;
    bool videoHaveFirst_ = false;
    bool audioHaveFirst_ = false;
    uint32_t lastVideoDts_ = 0;
    uint32_t lastAudioDts_ = 0;

    std::vector<Mp4BoxWriter::SampleInfo> videoSamples_;
    std::vector<uint64_t> videoDts_;  // 每 sample 的 dts(90k)，用于算 duration
    std::vector<uint8_t> videoMdat_;
    std::vector<Mp4BoxWriter::SampleInfo> audioSamples_;
    std::vector<uint64_t> audioDts_;
    std::vector<uint8_t> audioMdat_;
    uint32_t curVideoSegmentMs_ = 0;  // 当前分片起始相对 ms

    std::deque<uint32_t> segmentSeqs_;

    Mp4BoxWriter writer_;
    SegmentCallback onSegment_;

    AsyncFileWriter* asyncWriter_ = nullptr;
    uint64_t vodFileId_ = 0;  // VOD 模式下当前录制文件句柄
};

}  // namespace streaming
