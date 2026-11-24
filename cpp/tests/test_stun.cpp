#include "test_framework.hpp"
#include "webrtc/Stun.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace streaming;

#ifdef HAVE_OPENSSL
// RFC 5769 2.1 Sample Request（含 MESSAGE-INTEGRITY 与 FINGERPRINT）
TEST(stun_rfc5769_sample_request_integrity) {
    const uint8_t req[] = {
        0x00, 0x01, 0x00, 0x58, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01,
        0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x10,
        0x53, 0x54, 0x55, 0x4e, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x63, 0x6c,
        0x69, 0x65, 0x6e, 0x74, 0x00, 0x24, 0x00, 0x04, 0x6e, 0x00, 0x01, 0xff,
        0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36,
        0x00, 0x06, 0x00, 0x09, 0x65, 0x76, 0x74, 0x6a, 0x3a, 0x68, 0x36, 0x76,
        0x59, 0x20, 0x20, 0x20, 0x00, 0x08, 0x00, 0x14, 0x9a, 0xea, 0xa7, 0x0c,
        0xbf, 0xd8, 0xcb, 0x56, 0x78, 0x1e, 0xf2, 0xb5, 0xb2, 0xd3, 0xf2, 0x49,
        0xc1, 0xb5, 0x71, 0xa2, 0x80, 0x28, 0x00, 0x04, 0xe5, 0x7a, 0x3b, 0xcf};
    ASSERT_TRUE(Stun::checkIntegrity(req, sizeof(req), "VOkJxbRl1RmTxUk/WvJxBt"));
    ASSERT_TRUE(Stun::checkFingerprint(req, sizeof(req)));
    ASSERT_FALSE(Stun::checkIntegrity(req, sizeof(req), "wrong-password"));
}
#endif

#ifdef HAVE_OPENSSL
// 生成的 Binding Response 必须能被同一密码校验通过，且头部（类型/magic/事务 ID）正确
TEST(stun_binding_response_integrity_roundtrip) {
    StunMessage req;
    for (int i = 0; i < 12; ++i) req.txId[i] = static_cast<uint8_t>(i + 1);
    auto resp = Stun::buildBindingResponse(req, 0x7F000001, 50000, "server-pwd-123");
    ASSERT_TRUE(resp.size() > 20);
    ASSERT_EQ(resp[0], uint8_t(0x01));
    ASSERT_EQ(resp[1], uint8_t(0x01));
    ASSERT_EQ(resp[4], uint8_t(0x21));
    ASSERT_EQ(resp[8], uint8_t(1));
    ASSERT_TRUE(Stun::checkIntegrity(resp.data(), resp.size(), "server-pwd-123"));
    ASSERT_TRUE(Stun::checkFingerprint(resp.data(), resp.size()));
}
#endif

// ---- RTCP 接收报告 / REMB 解析与拥塞判断 ----
#include "webrtc/CongestionMonitor.hpp"
#include "webrtc/Rtcp.hpp"

TEST(rtcp_parse_receiver_report_blocks) {
    // RR，RC=1：sender ssrc=0x11111111，报告块 ssrc=0x22222222，fraction lost=64（25%）
    std::vector<uint8_t> rr = {0x81, 201, 0x00, 0x07, 0x11, 0x11, 0x11, 0x11,
                               0x22, 0x22, 0x22, 0x22, 64, 0, 0, 10,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    streaming::Rtcp::Feedback fb;
    ASSERT_TRUE(streaming::Rtcp::parse(rr.data(), rr.size(), fb));
    ASSERT_EQ(fb.reports.size(), static_cast<size_t>(1));
    ASSERT_EQ(fb.reports[0].ssrc, 0x22222222u);
    ASSERT_EQ(static_cast<int>(fb.reports[0].fractionLost), 64);
}

TEST(rtcp_parse_remb) {
    // PSFB fmt=15，"REMB"，1 个 SSRC，exp=2 mantissa=250000 → 1,000,000 bps
    uint32_t mant = 250000;
    std::vector<uint8_t> p = {0x8F, 206, 0x00, 0x05, 0, 0, 0, 1, 0, 0, 0, 0, 'R', 'E', 'M', 'B', 1,
                              static_cast<uint8_t>((2 << 2) | (mant >> 16)),
                              static_cast<uint8_t>((mant >> 8) & 0xFF), static_cast<uint8_t>(mant & 0xFF),
                              0xAA, 0xBB, 0xCC, 0xDD};
    streaming::Rtcp::Feedback fb;
    ASSERT_TRUE(streaming::Rtcp::parse(p.data(), p.size(), fb));
    ASSERT_TRUE(fb.isRemb);
    ASSERT_EQ(fb.rembBps, static_cast<uint64_t>(1000000));
    ASSERT_EQ(fb.rembSsrcs.size(), static_cast<size_t>(1));
    ASSERT_EQ(fb.rembSsrcs[0], 0xAABBCCDDu);
}

TEST(congestion_monitor_loss_and_remb) {
    streaming::CongestionMonitor m;
    // 源约 2 Mbps：每 100ms 25000 字节
    for (int t = 0; t <= 3000; t += 100) m.onSourceBytes(25000, t);
    ASSERT_TRUE(m.sourceBps() > 1500000 && m.sourceBps() < 2500000);
    ASSERT_FALSE(m.congested(3000));
    m.onReceiverReport(5, 3000);  // 2% 丢包：正常
    ASSERT_FALSE(m.congested(3001));
    m.onReceiverReport(64, 3100);  // 25% 丢包：拥塞
    ASSERT_TRUE(m.congested(3200));
    ASSERT_FALSE(m.congested(3100 + streaming::CongestionMonitor::kHoldMs));  // 无新信号则解除
    m.onRemb(3000000, 10000);  // 带宽充足
    ASSERT_FALSE(m.congested(10001));
    m.onRemb(1000000, 11000);  // 带宽低于源码率 80%
    ASSERT_TRUE(m.congested(11001));
    ASSERT_EQ(m.episodes(), static_cast<uint64_t>(2));
}
