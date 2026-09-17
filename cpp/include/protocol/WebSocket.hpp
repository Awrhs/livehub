#pragma once

#include "core/Buffer.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace streaming {

enum WsOpcode : uint8_t {
    kWsContinuation = 0x0,
    kWsText = 0x1,
    kWsBinary = 0x2,
    kWsClose = 0x8,
    kWsPing = 0x9,
    kWsPong = 0xA,
};

/**
 * WebSocket: RFC 6455 握手与帧编解码。
 *
 * 服务端：
 * - acceptKey() 把客户端 Sec-WebSocket-Key 与固定 GUID 拼接做 SHA1+Base64，
 *   生成 Sec-WebSocket-Accept。
 * - buildFrame() 构造服务端帧（服务端不 mask）。
 * - parseFrames() 解析客户端帧（必须 mask），通过回调返回 (opcode, payload)。
 *
 * 无 OpenSSL 时 SHA1 用内置实现，避免硬依赖。
 */
class WebSocket {
public:
    /// 根据客户端 Sec-WebSocket-Key 计算 accept 值
    static std::string acceptKey(const std::string& clientKey);

    /// 构造一帧（服务端到客户端，不 mask）
    static std::vector<uint8_t> buildFrame(WsOpcode opcode,
                                          const uint8_t* data, size_t len,
                                          bool fin = true);

    /// 构造 close 帧
    static std::vector<uint8_t> buildClose(uint16_t code = 1000);

    using FrameCallback =
        std::function<void(WsOpcode opcode, const uint8_t* data, size_t len)>;

    /// 从 buffer 解析 0 个或多个完整帧；消费已解析字节。
    /// 返回 false 表示协议错误（应关闭连接）。
    static bool parseFrames(Buffer& buf, const FrameCallback& cb);
};

}  // namespace streaming
