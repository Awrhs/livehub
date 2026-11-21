#pragma once

#include "core/Buffer.hpp"
#include "core/NonCopyable.hpp"

#include <cstdint>

namespace streaming {

/**
 * RTMP 握手状态机（服务端）。
 *
 * 简单握手：C0(1) + C1(1536) + C2(1536)；服务端回 S0/S1/S2，S2 回显 C1。
 * 复杂握手（FP9，FFmpeg/OBS/Flash 均使用）：
 *   C1 = time(4) + version(4) + 1528 字节，其中嵌 32 字节 digest，位置有两种 schema：
 *     schema A：pos = (b[8]+b[9]+b[10]+b[11]) % 728 + 12
 *     schema B：pos = (b[772]+b[773]+b[774]+b[775]) % 728 + 776
 *   digest = HMAC-SHA256(key, C1 去掉 digest 那 32 字节)
 *   C1 用 "Genuine Adobe Flash Player 001"(30B) 校验；S1 用 FMS key(36B) 生成；
 *   S2 = 随机 1504B + HMAC(HMAC(FMS 完整 key 68B, C1 digest), S2 前 1504B)。
 * 无 OpenSSL 或 C1 校验失败时降级简单握手（S1 version 置 0，客户端不会校验 S2）。
 */
class Handshake : NonCopyable {
public:
    enum State {
        kExpectC0C1,  // 等待 C0+C1
        kExpectC2,    // 已发 S0+S1+S2，等待 C2
        kDone,
        kError,       // 协议错误（如 C0 版本不支持），连接应关闭
    };

    Handshake();

    State state() const { return state_; }
    bool done() const { return state_ == kDone; }
    bool failed() const { return state_ == kError; }
    bool isComplex() const { return complex_; }

    /// 喂入接收到的数据，尽可能推进状态（可一次处理 C0C1 与 C2）。
    /// 握手应答写入 out。返回 true 表示握手已完成（done()）。
    /// 数据不足时保留未消费字节，等待下次调用。
    bool onRead(Buffer& in, Buffer& out);

    /// 计算 digest 位置（暴露给单测）
    static size_t digestPosSchemaA(const uint8_t* buf);
    static size_t digestPosSchemaB(const uint8_t* buf);
    /// HMAC-SHA256(key, buf[0..1536) 去掉 [pos, pos+32))
    static void calcDigest(const uint8_t* buf, size_t pos, const uint8_t* key,
                           size_t keyLen, uint8_t out[32]);
    /// 校验 C1 digest，成功返回位置，失败返回 0
    static size_t findValidDigest(const uint8_t* buf, const uint8_t* key,
                                  size_t keyLen);

    static void hmacSha256(const uint8_t* key, size_t keyLen,
                           const uint8_t* data, size_t dataLen,
                           uint8_t out[32]);

private:
    bool handleC0C1(Buffer& in, Buffer& out);
    bool handleC2(Buffer& in);
    void buildComplexS1S2(const uint8_t* c1, size_t c1DigestPos,
                          bool schemaB, Buffer& out);
    void buildSimpleS1S2(const uint8_t* c1, Buffer& out);

    State state_;
    bool complex_;
};

}  // namespace streaming
