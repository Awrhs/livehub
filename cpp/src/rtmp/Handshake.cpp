#include "rtmp/Handshake.hpp"
#include "core/Logger.hpp"

#include <cstring>
#include <ctime>

#ifdef HAVE_OPENSSL
#include <openssl/hmac.h>
#include <openssl/sha.h>
#endif

namespace streaming {

namespace {
constexpr const char* kServerVersion = "STREAM01";
constexpr uint8_t kFpVersion[] = {
    // Genuine FMS key prefix (用于复杂握手 HMAC key）
    0x47, 0x65, 0x6e, 0x75, 0x69, 0x6e, 0x65, 0x20,
    0x41, 0x64, 0x6f, 0x62, 0x65, 0x20, 0x46, 0x6c,
    0x61, 0x73, 0x68, 0x20, 0x4d, 0x65, 0x64, 0x69,
    0x61, 0x20, 0x53, 0x65, 0x72, 0x76, 0x65, 0x72,
};
}  // namespace

Handshake::Handshake() : state_(kExpectC0C1), complex_(false) {
    std::memset(s1_, 0, sizeof(s1_));
}

void Handshake::hmacSha256(const uint8_t* key, size_t keyLen,
                            const uint8_t* data, size_t dataLen,
                            uint8_t out[32]) {
#ifdef HAVE_OPENSSL
    unsigned int len = 32;
    HMAC(EVP_sha256(), key, static_cast<int>(keyLen), data, dataLen, out,
         &len);
#else
    // 无 OpenSSL：简单降级，不能保证复杂握手安全
    (void)key;
    (void)keyLen;
    (void)dataLen;
    for (int i = 0; i < 32; ++i) out[i] = data[i % dataLen] ^ 0x5A;
#endif
}

bool Handshake::isComplexC1(const uint8_t* c1) const {
    // C1 前 4 字节为 time，接下来 4 字节为 version。
    // version 非零（非 0x00000000）通常表示复杂握手。
    return c1[4] != 0 || c1[5] != 0 || c1[6] != 0 || c1[7] != 0;
}

bool Handshake::verifyC1Digest(const uint8_t* c1, int& digestOffset) const {
    // C1 = time(4) + zero/version(4) + random(1528)
    // random 被分成 digest block 与 key block。尝试两种 schema：
    //   schema 0: digest block 在前（off = 8）
    //   schema 1: key block 在前（digest off = 8 + 764 + (c1[8]%728)）
    static const char* digestKey = "Genuine Adobe Flash Media Server 001";
    static const size_t digestKeyLen = 36;

    int candidates[2];
    candidates[0] = 8;
    candidates[1] = 8 + 764 + (static_cast<int>(c1[8]) % 728);

    for (int off : candidates) {
        if (off < 0 || off + 764 > 1536) continue;
        // digest block 结构：offset(4) + random-data(...) + digest(32)
        // 简化：HMAC(c1[off..off+732] 作为 data，digestKey) 与末尾 32 字节比较
        const uint8_t* block = c1 + off;
        uint8_t calc[32];
        hmacSha256(reinterpret_cast<const uint8_t*>(digestKey), digestKeyLen,
                   block, 764 - 32, calc);
        if (std::memcmp(calc, block + 764 - 32, 32) == 0) {
            digestOffset = off;
            return true;
        }
    }
    return false;
}

void Handshake::buildS1S2(const uint8_t* c1, Buffer& out) {
    // S1
    uint8_t s1[1536];
    std::memset(s1, 0, sizeof(s1));
    uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    s1[0] = (now >> 24) & 0xFF;
    s1[1] = (now >> 16) & 0xFF;
    s1[2] = (now >> 8) & 0xFF;
    s1[3] = now & 0xFF;
    std::memcpy(s1 + 4, kServerVersion, 8);
    // 填充随机数据（用 c1 派生，简单方案）
    for (int i = 8; i < 1536; ++i) {
        s1[i] = static_cast<uint8_t>(c1[i] ^ 0x36);
    }

    // 在 digest block 中写入合法 digest（schema 0，offset=8）
    static const char* digestKey = "Genuine Adobe Flash Media Server 001";
    uint8_t calc[32];
    hmacSha256(reinterpret_cast<const uint8_t*>(digestKey), 36,
               s1 + 8, 764 - 32, calc);
    std::memcpy(s1 + 8 + 764 - 32, calc, 32);

    // S2 = 回显 C1
    out.append(reinterpret_cast<const char*>(s1), 1536);
    out.append(reinterpret_cast<const char*>(c1), 1536);
}

bool Handshake::handleC0C1(Buffer& in, Buffer& out) {
    if (in.readableBytes() < 1 + 1536) return false;

    uint8_t c0 = static_cast<uint8_t>(in.readInt8());
    if (c0 != 3) {
        LOG_WARN("RTMP handshake: unsupported C0 version %d", c0);
        return false;
    }

    uint8_t c1[1536];
    if (in.readableBytes() < 1536) return false;
    std::memcpy(c1, in.peek(), 1536);
    in.retrieve(1536);

    // S0
    out.appendInt8(3);

    complex_ = isComplexC1(c1);
    if (complex_) {
        int digestOff = -1;
        if (verifyC1Digest(c1, digestOff)) {
            LOG_DEBUG("RTMP complex handshake verified, digest off=%d",
                      digestOff);
            buildS1S2(c1, out);
        } else {
            // 未通过校验，降级为简单握手
            complex_ = false;
            LOG_DEBUG("RTMP C1 digest verify failed, fallback to simple");
        }
    }
    if (!complex_) {
        // 简单握手：S1 = 服务端时间戳 + 版本 + 随机；S2 = 回显 C1
        uint8_t s1[1536];
        std::memset(s1, 0, sizeof(s1));
        uint32_t now = static_cast<uint32_t>(std::time(nullptr));
        s1[0] = (now >> 24) & 0xFF;
        s1[1] = (now >> 16) & 0xFF;
        s1[2] = (now >> 8) & 0xFF;
        s1[3] = now & 0xFF;
        for (int i = 8; i < 1536; ++i) {
            s1[i] = static_cast<uint8_t>(c1[i] ^ 0x5A);
        }
        out.append(reinterpret_cast<const char*>(s1), 1536);  // S1
        out.append(reinterpret_cast<const char*>(c1), 1536);  // S2 = echo C1
    }

    std::memcpy(s1_, c1, 1536);
    state_ = kExpectC2;
    return true;
}

bool Handshake::handleC2(Buffer& in, Buffer& /*out*/) {
    if (in.readableBytes() < 1536) return false;
    in.retrieve(1536);  // C2 内容通常回显 S1，不校验
    state_ = kDone;
    return true;
}

bool Handshake::onRead(Buffer& in, Buffer& out) {
    if (state_ == kExpectC0C1) {
        return handleC0C1(in, out);
    } else if (state_ == kExpectC2) {
        return handleC2(in, out);
    }
    return false;
}

}  // namespace streaming
