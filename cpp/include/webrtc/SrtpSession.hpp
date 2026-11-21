#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace streaming {

/**
 * SrtpSession: RTP/RTCP 加解密（RFC 3711）。
 *
 * 持有发送/接收方向的 SRTP 上下文，对 RTP 报文做 AES-128 加解密与 HMAC
 * 认证。key/salt 来自 DTLS 导出的 SRTP master material。
 *
 * 编译期若未检测到 libsrtp2，则退化为透传（不加密，仅供骨架联调）。
 */
class SrtpSession {
public:
    SrtpSession();
    ~SrtpSession();

    /// profile：DTLS-SRTP 协商结果（1 = AES_CM_128_HMAC_SHA1_80，7 = AEAD_AES_128_GCM）
    static constexpr int kAes128CmSha1_80 = 1;
    static constexpr int kAeadAes128Gcm = 7;

    /// 初始化发送方向（本端发出的 RTP/RTCP 加密）
    bool initSend(int profile, const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& salt);
    /// 初始化接收方向（对端发来的 RTP/RTCP 解密）
    bool initRecv(int profile, const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& salt);

    /// 保护（加密）一个 RTP 报文，原地写入密文，返回新长度；失败返回 -1
    int protectRtp(uint8_t* data, int len, int cap);
    /// 解保护（解密）一个 RTP 报文，原地写入明文，返回明文长度；失败返回 -1
    int unprotectRtp(uint8_t* data, int len);

    int protectRtcp(uint8_t* data, int len, int cap);
    int unprotectRtcp(uint8_t* data, int len);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace streaming
