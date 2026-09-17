#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace streaming {

/**
 * SrtProtocol: SRT (Secure Reliable Transport) 协议常量与报文编解码。
 *
 * SRT 基于 UDP，报文头 16 字节：
 *  - 数据包：bit31=0，seq(31 bits)
 *  - 控制包：bit31=1，type(15 bits)
 *
 * 这里实现握手（caller/listener 简化版）、ACK/ACKACK、NAK 丢包重传、
 * KEEPALIVE、SHUTDOWN；媒体负载为 188 字节 MPEG-TS（SRT 直播默认）。
 *
 * 参考：SRT RFC-draft (Access Devices Group / Haivision)。
 */

// SRT UDP 报文固定头长度
constexpr size_t kSrtHeaderLen = 16;

// 包类型判定（首字节最高位）
inline bool srtIsControl(const uint8_t* p) { return (p[0] & 0x80) != 0; }

// ---- 控制包类型（type 字段，15-bit）----
enum class SrtCtrlType : uint16_t {
    kHandshake = 0x0000,
    kKeepAlive = 0x0001,
    kAck = 0x0002,
    kNak = 0x0003,
    kCongestion = 0x0004,
    kShutdown = 0x0005,
    kAckAck = 0x0006,
    kDropReq = 0x0007,
    kPeerError = 0x0008,
};

// 握手阶段（HS.type）
enum class SrtHandshakeType : uint32_t {
    kCaller = 0x00000001,     // 呼叫方（推流端）
    kResponder = 0x00000002,  // 应答方（接收端）
    kDone = 0xFFFFFFFF,       // 握手完成确认
};

constexpr uint32_t kSrtVersion = 0x00010500;  // 1.5.0
constexpr uint32_t kSrtExtMagic = 0x4A52;     // SRT 扩展 magic "JR"
constexpr uint32_t kDefaultMtu = 1500;
constexpr uint32_t kDefaultMaxFlowWindow = 8192;
constexpr uint32_t kDefaultInitialSeq = 1;

#pragma pack(push, 1)
struct SrtHeader {
    uint32_t field0;       // 数据包: bit31=0 + seqno; 控制包: bit31=1 + type(15)
    uint32_t timestamp;    // 相对启动时间，微秒
    uint32_t destSocket;   // 对端 socket id
};
#pragma pack(pop)

// 数据包序号（低 31 位）
uint32_t srtDataSeq(const uint8_t* p);

// 控制包 type（低 15 位）
uint16_t srtCtrlType(const uint8_t* p);

// 构造一个控制包（仅 16 字节头，附加信息由调用方 append 到 field0.info 区域）
std::vector<uint8_t> srtBuildControl(SrtCtrlType type, uint32_t info,
                                     uint32_t timestamp, uint32_t destSocket);

// 构造一个数据包（头 + 负载）
std::vector<uint8_t> srtBuildData(uint32_t seqno, uint32_t timestamp,
                                  uint32_t srcSocket, const uint8_t* payload,
                                  size_t len);

// 相对启动时间（微秒）
uint32_t srtNowUs();

}  // namespace streaming
