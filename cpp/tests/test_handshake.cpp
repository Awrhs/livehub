#include "core/Buffer.hpp"
#include "rtmp/Handshake.hpp"
#include "test_framework.hpp"

#include <cstring>
#include <vector>

using namespace streaming;

namespace {
const uint8_t kTail[32] = {
    0xF0, 0xEE, 0xC2, 0x4A, 0x80, 0x68, 0xBE, 0xE8, 0x2E, 0x00, 0xD0,
    0xD1, 0x02, 0x9E, 0x7E, 0x57, 0x6E, 0xEC, 0x5D, 0x2D, 0x29, 0x80,
    0x6F, 0xAB, 0x93, 0xB8, 0xE6, 0x36, 0xCF, 0xEB, 0x31, 0xAE};
std::vector<uint8_t> key(const char* s) {
    std::vector<uint8_t> v(s, s + std::strlen(s));
    v.insert(v.end(), kTail, kTail + 32);
    return v;
}
}  // namespace

// 简单握手：C0C1 与 C2 一次到达，应一次完成且不残留字节（回归：原实现不等 C2）
TEST(handshake_simple_c0c1c2_in_one_read) {
    Handshake hs;
    Buffer in, out;
    std::vector<uint8_t> c(1 + 1536 + 1536, 0x11);
    c[0] = 3;
    std::memset(c.data() + 1 + 4, 0, 4);  // version 0 → 简单握手
    in.append(reinterpret_cast<const char*>(c.data()), c.size());
    ASSERT_TRUE(hs.onRead(in, out));
    ASSERT_TRUE(hs.done());
    ASSERT_EQ(in.readableBytes(), size_t(0));
    ASSERT_EQ(out.readableBytes(), size_t(1 + 1536 + 1536));
}

// 只收到 C0C1 时不能报告完成
TEST(handshake_waits_for_c2) {
    Handshake hs;
    Buffer in, out;
    std::vector<uint8_t> c(1 + 1536, 0x22);
    c[0] = 3;
    in.append(reinterpret_cast<const char*>(c.data()), c.size());
    ASSERT_FALSE(hs.onRead(in, out));
    ASSERT_FALSE(hs.done());
    ASSERT_EQ(hs.state(), Handshake::kExpectC2);
    std::vector<uint8_t> c2(1536, 0x33);
    in.append(reinterpret_cast<const char*>(c2.data()), c2.size());
    ASSERT_TRUE(hs.onRead(in, out));
    ASSERT_TRUE(hs.done());
}

TEST(handshake_bad_version_fails) {
    Handshake hs;
    Buffer in, out;
    std::vector<uint8_t> c(1 + 1536, 0);
    c[0] = 6;  // RTMPE
    in.append(reinterpret_cast<const char*>(c.data()), c.size());
    ASSERT_FALSE(hs.onRead(in, out));
    ASSERT_TRUE(hs.failed());
}

#ifdef HAVE_OPENSSL
// 复杂握手：按 FFmpeg 客户端的方式构造 C1 并校验 S1 digest 与 S2 签名
TEST(handshake_complex_ffmpeg_compatible) {
    auto fpKey = key("Genuine Adobe Flash Player 001");
    auto fmsKey = key("Genuine Adobe Flash Media Server 001");

    uint8_t c1[1536];
    for (int i = 0; i < 1536; ++i) c1[i] = uint8_t(i * 7 + 3);
    c1[4] = 9; c1[5] = 0; c1[6] = 124; c1[7] = 2;  // FFmpeg 客户端版本
    size_t pos = Handshake::digestPosSchemaA(c1);
    uint8_t d[32];
    Handshake::calcDigest(c1, pos, fpKey.data(), 30, d);
    std::memcpy(c1 + pos, d, 32);

    Handshake hs;
    Buffer in, out;
    char c0 = 3;
    in.append(&c0, 1);
    in.append(reinterpret_cast<const char*>(c1), 1536);
    hs.onRead(in, out);
    ASSERT_TRUE(hs.isComplex());
    ASSERT_EQ(out.readableBytes(), size_t(1 + 1536 * 2));

    const uint8_t* s = reinterpret_cast<const uint8_t*>(out.peek());
    ASSERT_EQ(s[0], uint8_t(3));
    const uint8_t* s1 = s + 1;
    const uint8_t* s2 = s + 1 + 1536;
    ASSERT_TRUE(s1[4] >= 3);  // FFmpeg 据此决定是否校验
    // S1 digest 用 FMS key(36) 可校验
    ASSERT_TRUE(Handshake::findValidDigest(s1, fmsKey.data(), 36) != 0);
    // S2 签名 = HMAC(HMAC(FMS 68B, C1 digest), S2[0..1504))
    uint8_t tmp[32], sig[32];
    Handshake::hmacSha256(fmsKey.data(), fmsKey.size(), c1 + pos, 32, tmp);
    Handshake::hmacSha256(tmp, 32, s2, 1536 - 32, sig);
    ASSERT_TRUE(std::memcmp(sig, s2 + 1536 - 32, 32) == 0);
}
#endif
