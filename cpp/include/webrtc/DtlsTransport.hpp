#pragma once

#include "net/EventLoop.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace streaming {

/**
 * DtlsTransport: DTLS 1.2 握手 + SRTP keying material 导出。
 *
 * 用于 WebRTC 数据通道的加密协商。握手完成后通过 onReady 回调把
 * SRTP master key/salt 交给 SrtpSession 用于 RTP/RTCP 加解密。
 *
 * 实现细节：用 OpenSSL 的内存 BIO（自定义 pair），把 DTLS record 通过
 * UDP 发送；接收侧 feed() 喂入密文，SSL_* 驱动握手/解密。
 *
 * 无 OpenSSL 时为 stub：记录指纹透传，握手标记为不加密（仅供协议骨架联调）。
 */
class DtlsTransport {
public:
    enum class Role { kClient, kServer };

    using SendCallback = std::function<void(const uint8_t*, size_t)>;
    /// DTLS-SRTP 导出的密钥（RFC 5764 4.2），按本端角色区分收发方向
    struct SrtpKeys {
        int profile = 0;                    // SRTP 保护 profile（1=AES_CM_SHA1_80，7=AEAD_AES_128_GCM）
        std::vector<uint8_t> sendKey, sendSalt;  // 本端发送（加密）
        std::vector<uint8_t> recvKey, recvSalt;  // 对端发来（解密）
    };
    using ReadyCallback = std::function<void(const SrtpKeys&)>;

    DtlsTransport(Role role, std::string localFingerprint);
    ~DtlsTransport();

    void setSendCallback(SendCallback cb) { sendCb_ = std::move(cb); }
    void setReadyCallback(ReadyCallback cb) { readyCb_ = std::move(cb); }

    /// 初始化（加载证书/私钥）。无 OpenSSL 时返回 true 并进入「透传」模式。
    bool init(const std::string& certPem = "", const std::string& keyPem = "");

    /// 启动握手（服务端等 ClientHello，客户端主动发）
    void startHandshake();

    /// 喂入对端 DTLS record
    void feed(const uint8_t* data, size_t len);

    bool established() const { return established_; }

    /// 本地证书的 SHA-256 指纹（"A1:B2:..."），供 SDP a=fingerprint
    static std::string localFingerprintSha256(const std::string& certPem = "");

    void setRemoteFingerprint(std::string fp) {
        remoteFingerprint_ = std::move(fp);
    }

private:
    void sendPending();
    void doHandshakeStep();

    Role role_;
    std::string localFingerprint_;
    SendCallback sendCb_;
    ReadyCallback readyCb_;
    bool established_ = false;

    struct Impl;
    Impl* impl_ = nullptr;

    // 对端 DTLS 指纹（SDP a=fingerprint），用于校验（可选）
    std::string remoteFingerprint_;
};

}  // namespace streaming
