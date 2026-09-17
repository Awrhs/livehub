#pragma once

#include "core/NonCopyable.hpp"
#include "core/Buffer.hpp"

#include <cstdint>

namespace streaming {

/**
 * RTMP 握手状态机。
 *
 * 简单握手：C0(1) + C1(1536) + C2(1536)；服务端回 S0/S1/S2。
 * 复杂握手：C1 由 764 字节 key block + 764 字节 digest block 组成，
 *           用 HMAC-SHA256 计算并校验 digest（FP 方案）。
 *
 * 有 OpenSSL 时做复杂握手（兼容 Flash/ffmpeg 严格校验），无则降级简单握手。
 *
 * 简历亮点：二进制状态机、HMAC-SHA256 复杂握手校验
 */
class Handshake : NonCopyable {
public:
    enum State {
        kExpectC0C1,  // 等待 C0+C1
        kExpectC2,    // 已发 S0+S1+S2，等待 C2
        kDone,
    };

    Handshake();

    State state() const { return state_; }
    bool done() const { return state_ == kDone; }

    /// 喂入接收到的数据，返回 true 表示状态推进；握手过程中产生的应答写入 out。
    /// 数据不足时保留状态等待下次。
    bool onRead(Buffer& in, Buffer& out);

private:
    bool handleC0C1(Buffer& in, Buffer& out);
    bool handleC2(Buffer& in, Buffer& out);

    // 复杂握手
    bool isComplexC1(const uint8_t* c1) const;
    bool verifyC1Digest(const uint8_t* c1, int& digestOffset) const;
    void buildS1S2(const uint8_t* c1, Buffer& out);

    static void hmacSha256(const uint8_t* key, size_t keyLen,
                            const uint8_t* data, size_t dataLen,
                            uint8_t out[32]);

    State state_;
    uint8_t s1_[1536];
    bool complex_;
};

}  // namespace streaming
