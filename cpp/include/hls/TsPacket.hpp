#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace streaming {

/**
 * MPEG-TS 常量
 */
constexpr int kTsPacketSize = 188;
constexpr uint8_t kTsSync = 0x47;

constexpr uint16_t kPatPid = 0x0000;
constexpr uint16_t kDefaultPmtPid = 0x1000;
constexpr uint16_t kDefaultVideoPid = 0x0100;
constexpr uint16_t kDefaultAudioPid = 0x0101;

constexpr uint8_t kStreamTypeVideo = 0x1B;  // AVC (H.264)
constexpr uint8_t kStreamTypeHevc = 0x24;   // HEVC (H.265)
constexpr uint8_t kStreamTypeAudio = 0x0F;  // AAC ADTS

/**
 * TsPacket: 188 字节 MPEG-TS 包构造工具。
 *
 * TS 包结构：
 *   4 字节 header（sync + TEI/PUSI/优先级 + PID + TSC/AFC/CC）
 *   adaptation field（可选，含 PCR / stuffing）
 *   payload
 *
 * 简历亮点：手写 TS 打包、CRC32、PCR、continuity counter
 */
class TsPacket {
public:
    /// MPEG-2 CRC32（多项式 0x04C11DB7，初值 0xFFFFFFFF）
    static uint32_t crc32(const uint8_t* data, size_t len);

    /// 构造 PAT 包（节目关联表）。pmtPid 指向 PMT 的 PID。
    static size_t buildPat(uint8_t* out, uint16_t pmtPid, uint8_t cc);

    /// 构造 PMT 包（节目映射表）。videoStreamType 取 kStreamTypeVideo/kStreamTypeHevc。
    static size_t buildPmt(uint8_t* out, uint16_t pmtPid, uint16_t pcrPid,
                           uint16_t videoPid, uint16_t audioPid, uint8_t cc,
                           uint8_t videoStreamType = kStreamTypeVideo);

    /// 构造承载 PES 的 TS 包。
    /// @param payload PES 数据（从 0x000001 开始）
    /// @param pusi 是否是 payload unit start indicator（PES 首包为 true）
    /// @param pcr 非负值时写入 PCR（单位：27MHz/300 即 90kHz 基准的 300 倍）
    /// @param randomAccess 关键帧（置 adaptation field random_access_indicator）
    static size_t buildPes(uint8_t* out, uint16_t pid, uint8_t cc, bool pusi,
                           const uint8_t* payload, size_t payloadLen,
                           int64_t pcr = -1, bool randomAccess = false,
                           size_t* consumed = nullptr);

private:
    static void writeHeader(uint8_t* out, uint16_t pid, uint8_t cc,
                            bool pusi, bool hasAdaptation, bool hasPayload);
};

}  // namespace streaming
