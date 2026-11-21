#include "rtmp/Handshake.hpp"

#include "core/Logger.hpp"

#include <cstring>
#include <ctime>
#include <random>
#include <vector>

#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

namespace streaming {

namespace {
constexpr size_t kHsSize = 1536;

// 公开的 RTMP 握手常量（FFmpeg rtmpproto.c / SRS 同款）
constexpr uint8_t kKeyTail[32] = {
    0xF0, 0xEE, 0xC2, 0x4A, 0x80, 0x68, 0xBE, 0xE8, 0x2E, 0x00, 0xD0,
    0xD1, 0x02, 0x9E, 0x7E, 0x57, 0x6E, 0xEC, 0x5D, 0x2D, 0x29, 0x80,
    0x6F, 0xAB, 0x93, 0xB8, 0xE6, 0x36, 0xCF, 0xEB, 0x31, 0xAE};

const std::vector<uint8_t>& playerKey() {  // 30 + 32
    static const std::vector<uint8_t> k = [] {
        const char* s = "Genuine Adobe Flash Player 001";
        std::vector<uint8_t> v(s, s + 30);
        v.insert(v.end(), kKeyTail, kKeyTail + 32);
        return v;
    }();
    return k;
}
const std::vector<uint8_t>& serverKey() {  // 36 + 32
    static const std::vector<uint8_t> k = [] {
        const char* s = "Genuine Adobe Flash Media Server 001";
        std::vector<uint8_t> v(s, s + 36);
        v.insert(v.end(), kKeyTail, kKeyTail + 32);
        return v;
    }();
    return k;
}

void fillRandom(uint8_t* p, size_t n) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(rng());
}
}  // namespace

Handshake::Handshake() : state_(kExpectC0C1), complex_(false) {}

void Handshake::hmacSha256(const uint8_t* key, size_t keyLen,
                           const uint8_t* data, size_t dataLen,
                           uint8_t out[32]) {
#ifdef HAVE_OPENSSL
    unsigned int len = 32;
    HMAC(EVP_sha256(), key, static_cast<int>(keyLen), data, dataLen, out,
         &len);
#else
    (void)key;
    (void)keyLen;
    (void)data;
    (void)dataLen;
    std::memset(out, 0, 32);  // 无 OpenSSL 时不做复杂握手，此函数不会被依赖
#endif
}

size_t Handshake::digestPosSchemaA(const uint8_t* b) {
    return (size_t(b[8]) + b[9] + b[10] + b[11]) % 728 + 12;
}

size_t Handshake::digestPosSchemaB(const uint8_t* b) {
    return (size_t(b[772]) + b[773] + b[774] + b[775]) % 728 + 776;
}

void Handshake::calcDigest(const uint8_t* buf, size_t pos, const uint8_t* key,
                           size_t keyLen, uint8_t out[32]) {
    uint8_t tmp[kHsSize - 32];
    std::memcpy(tmp, buf, pos);
    std::memcpy(tmp + pos, buf + pos + 32, kHsSize - pos - 32);
    hmacSha256(key, keyLen, tmp, sizeof(tmp), out);
}

size_t Handshake::findValidDigest(const uint8_t* buf, const uint8_t* key,
                                  size_t keyLen) {
    for (size_t pos : {digestPosSchemaA(buf), digestPosSchemaB(buf)}) {
        uint8_t d[32];
        calcDigest(buf, pos, key, keyLen, d);
        if (std::memcmp(d, buf + pos, 32) == 0) return pos;
    }
    return 0;
}

void Handshake::buildSimpleS1S2(const uint8_t* c1, Buffer& out) {
    uint8_t s1[kHsSize];
    uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    s1[0] = (now >> 24) & 0xFF;
    s1[1] = (now >> 16) & 0xFF;
    s1[2] = (now >> 8) & 0xFF;
    s1[3] = now & 0xFF;
    std::memset(s1 + 4, 0, 4);  // version=0：告知客户端为简单握手，不校验 S2
    fillRandom(s1 + 8, kHsSize - 8);
    out.append(reinterpret_cast<const char*>(s1), kHsSize);  // S1
    out.append(reinterpret_cast<const char*>(c1), kHsSize);  // S2 = 回显 C1
}

void Handshake::buildComplexS1S2(const uint8_t* c1, size_t c1DigestPos,
                                 bool schemaB, Buffer& out) {
    // ---- S1 ----
    uint8_t s1[kHsSize];
    uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    s1[0] = (now >> 24) & 0xFF;
    s1[1] = (now >> 16) & 0xFF;
    s1[2] = (now >> 8) & 0xFF;
    s1[3] = now & 0xFF;
    // 服务器版本（与 FMS 惯例一致，>=3 表示支持复杂握手）
    s1[4] = 0x0D;
    s1[5] = 0x0E;
    s1[6] = 0x0A;
    s1[7] = 0x0D;
    fillRandom(s1 + 8, kHsSize - 8);
    size_t s1Pos = schemaB ? digestPosSchemaB(s1) : digestPosSchemaA(s1);
    uint8_t s1Digest[32];
    calcDigest(s1, s1Pos, serverKey().data(), 36, s1Digest);
    std::memcpy(s1 + s1Pos, s1Digest, 32);

    // ---- S2 ----
    uint8_t s2[kHsSize];
    fillRandom(s2, kHsSize);
    uint8_t tempKey[32];
    hmacSha256(serverKey().data(), serverKey().size(), c1 + c1DigestPos, 32,
               tempKey);
    uint8_t sig[32];
    hmacSha256(tempKey, 32, s2, kHsSize - 32, sig);
    std::memcpy(s2 + kHsSize - 32, sig, 32);

    out.append(reinterpret_cast<const char*>(s1), kHsSize);
    out.append(reinterpret_cast<const char*>(s2), kHsSize);
}

bool Handshake::handleC0C1(Buffer& in, Buffer& out) {
    if (in.readableBytes() < 1 + kHsSize) return false;

    const uint8_t c0 = static_cast<uint8_t>(in.peek()[0]);
    if (c0 != 3) {
        // 6 = RTMPE（加密）等，不支持
        LOG_WARN("RTMP handshake: unsupported C0 version %d", c0);
        state_ = kError;
        return false;
    }
    uint8_t c1[kHsSize];
    std::memcpy(c1, in.peek() + 1, kHsSize);
    in.retrieve(1 + kHsSize);

    out.appendInt8(3);  // S0

    complex_ = false;
#ifdef HAVE_OPENSSL
    // C1 version 非 0 才可能是复杂握手
    if (c1[4] || c1[5] || c1[6] || c1[7]) {
        size_t pos = findValidDigest(c1, playerKey().data(), 30);
        if (pos) {
            complex_ = true;
            bool schemaB = (pos == digestPosSchemaB(c1));
            buildComplexS1S2(c1, pos, schemaB, out);
            LOG_DEBUG("RTMP complex handshake (schema %c)", schemaB ? 'B' : 'A');
        } else {
            LOG_DEBUG("RTMP C1 digest not found, fallback to simple handshake");
        }
    }
#endif
    if (!complex_) buildSimpleS1S2(c1, out);

    state_ = kExpectC2;
    return true;
}

bool Handshake::handleC2(Buffer& in) {
    if (in.readableBytes() < kHsSize) return false;
    in.retrieve(kHsSize);  // C2 通常回显 S1，宽松起见不校验
    state_ = kDone;
    return true;
}

bool Handshake::onRead(Buffer& in, Buffer& out) {
    // C0C1 与 C2 可能在同一次 read 中到达，循环推进
    for (;;) {
        if (state_ == kExpectC0C1) {
            if (!handleC0C1(in, out)) return false;
        } else if (state_ == kExpectC2) {
            if (!handleC2(in)) return false;
        } else {
            return state_ == kDone;
        }
    }
}

}  // namespace streaming
