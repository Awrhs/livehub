#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace streaming {

// STUN (RFC 5389) message type / method
enum : uint16_t {
    kStunBindingRequest = 0x0001,
    kStunBindingResponse = 0x0101,
    kStunBindingError = 0x0111,
};
// Attribute types
enum : uint16_t {
    kStunAttrMappedAddress = 0x0001,
    kStunAttrUsername = 0x0006,
    kStunAttrMessageIntegrity = 0x0008,
    kStunAttrErrorCode = 0x0009,
    kStunAttrUnknownAttributes = 0x000A,
    kStunAttrXorMappedAddress = 0x0020,
    kStunAttrSoftware = 0x8022,
    kStunAttrFingerprint = 0x8028,
    kStunAttrIceControlled = 0x8029,
    kStunAttrIceControlling = 0x802A,
    kStunAttrPriority = 0x0024,
    kStunAttrUseCandidate = 0x0025,
};

/**
 * 解析后的 STUN 消息。
 */
struct StunMessage {
    uint16_t type = 0;
    uint16_t length = 0;
    uint8_t magic[4] = {0x21, 0x12, 0xA4, 0x42};
    uint8_t txId[12] = {};

    struct Attr {
        uint16_t type;
        std::vector<uint8_t> value;
    };
    std::vector<Attr> attrs;

    const Attr* find(uint16_t type) const;
    std::string username() const;
    uint32_t priority() const;
    bool hasUseCandidate() const;
};

/**
 * Stun: 轻量 STUN 编解码 + HMAC 完整性校验（OpenSSL 可选）。
 *
 * 用于 ICE-lite connectivity check 与 XOR-MAPPED-ADDRESS 应答。
 */
class Stun {
public:
    static bool parse(const uint8_t* data, size_t len, StunMessage& out);

    /// 构造 Binding Success Response，含 XOR-MAPPED-ADDRESS、
    /// MESSAGE-INTEGRITY（key 非空时）、FINGERPRINT。
    static std::vector<uint8_t> buildBindingResponse(
        const StunMessage& req, uint32_t addrIpv4, uint16_t port,
        const std::string& password = "", bool addFingerprint = true);

    /// 构造 ICE connectivity check（Binding Request），含
    /// USERNAME / PRIORITY / ICE-CONTROLLING / USE-CANDIDATE /
    /// MESSAGE-INTEGRITY（password 非空）/ FINGERPRINT。
    static std::vector<uint8_t> buildBindingRequest(
        const std::string& username, uint32_t priority,
        uint64_t tieBreaker, bool useCandidate,
        const std::string& password = "");

    /// STUN 长/短期认证的 HMAC-SHA1 完整性校验（OpenSSL 可用时）
    /// 校验 FINGERPRINT（RFC 5389 15.5）；无该属性返回 false
    static bool checkFingerprint(const uint8_t* data, size_t len);

    static bool checkIntegrity(const uint8_t* data, size_t len,
                               const std::string& password);
};

}  // namespace streaming
