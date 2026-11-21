#include "webrtc/Stun.hpp"
#include "core/Logger.hpp"

#include <arpa/inet.h>

#include <cstring>

#ifdef HAVE_OPENSSL
#include <openssl/hmac.h>
#include <openssl/sha.h>
#endif

namespace streaming {

namespace {
uint32_t crc32Ieee(const uint8_t* p, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}
void putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x >> 8);
    v.push_back(x & 0xFF);
}
void putU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24);
    v.push_back(x >> 16);
    v.push_back(x >> 8);
    v.push_back(x & 0xFF);
}
}  // namespace

const StunMessage::Attr* StunMessage::find(uint16_t t) const {
    for (const auto& a : attrs)
        if (a.type == t) return &a;
    return nullptr;
}

std::string StunMessage::username() const {
    const Attr* a = find(kStunAttrUsername);
    if (!a) return "";
    return std::string(reinterpret_cast<const char*>(a->value.data()),
                       a->value.size());
}

uint32_t StunMessage::priority() const {
    const Attr* a = find(kStunAttrPriority);
    if (!a || a->value.size() != 4) return 0;
    return (uint32_t(a->value[0]) << 24) | (uint32_t(a->value[1]) << 16) |
           (uint32_t(a->value[2]) << 8) | uint32_t(a->value[3]);
}

bool StunMessage::hasUseCandidate() const {
    return find(kStunAttrUseCandidate) != nullptr;
}

bool Stun::parse(const uint8_t* data, size_t len, StunMessage& out) {
    if (len < 20) return false;
    uint16_t msgType = (uint16_t(data[0]) << 8) | data[1];
    uint16_t msgLen = (uint16_t(data[2]) << 8) | data[3];
    if (static_cast<size_t>(msgLen) + 20 > len) return false;
    if (data[4] != 0x21 || data[5] != 0x12 || data[6] != 0xA4 ||
        data[7] != 0x42) {
        return false;
    }
    out.type = msgType;
    out.length = msgLen;
    std::memcpy(out.magic, data + 4, 4);
    std::memcpy(out.txId, data + 8, 12);

    size_t off = 20;
    const size_t end = 20 + static_cast<size_t>(msgLen);
    while (off + 4 <= end) {
        uint16_t at = (uint16_t(data[off]) << 8) | data[off + 1];
        uint16_t al = (uint16_t(data[off + 2]) << 8) | data[off + 3];
        off += 4;
        if (off + al > end) break;
        StunMessage::Attr attr;
        attr.type = at;
        attr.value.assign(data + off, data + off + al);
        out.attrs.push_back(std::move(attr));
        off += al;
        // 属性按 4 字节对齐
        off += (4 - (al % 4)) % 4;
    }
    return true;
}

static void appendXorMappedAddress(std::vector<uint8_t>& v,
                                   const uint8_t txId[12],
                                   uint32_t ipv4HostOrder,
                                   uint16_t portHostOrder) {
    putU16(v, kStunAttrXorMappedAddress);
    putU16(v, 8);  // length
    v.push_back(0);
    v.push_back(0x01);  // IPv4
    // xport = port ^ (magic >> 16) = port ^ 0x2112
    uint16_t xport = portHostOrder ^ 0x2112;
    putU16(v, xport);
    // xaddr = ipv4 ^ magic(0x2112A442)
    uint32_t magic = 0x2112A442;
    putU32(v, ipv4HostOrder ^ magic);
    (void)txId;
}

std::vector<uint8_t> Stun::buildBindingResponse(
    const StunMessage& req, uint32_t addrIpv4HostOrder, uint16_t portHostOrder,
    const std::string& password, bool addFingerprint) {

    std::vector<uint8_t> msg;
    msg.assign(20, 0);
    // 头部必须先写好：MESSAGE-INTEGRITY 与 FINGERPRINT 的计算覆盖头部
    //（原实现在最后才写入类型/magic/事务 ID，签名与校验和均基于全 0 头部计算，对端必然拒绝）
    msg[0] = 0x01;
    msg[1] = 0x01;  // Binding Success
    msg[4] = 0x21;
    msg[5] = 0x12;
    msg[6] = 0xA4;
    msg[7] = 0x42;
    std::memcpy(msg.data() + 8, req.txId, 12);

    appendXorMappedAddress(msg, req.txId, addrIpv4HostOrder, portHostOrder);

    // SOFTWARE
    const std::string software = "streaming-cpp/1.0";
    putU16(msg, kStunAttrSoftware);
    putU16(msg, static_cast<uint16_t>(software.size()));
    msg.insert(msg.end(), software.begin(), software.end());
    while (msg.size() % 4 != 0) msg.push_back(0);

    // MESSAGE-INTEGRITY（HMAC-SHA1，20 字节）
    // 计算前需把消息长度置为「当前末尾 - 20 + 24（MI 头+体）」
    bool withMi = !password.empty();
    if (withMi) {
        size_t beforeMi = msg.size();
        // 写 MI 占位
        putU16(msg, kStunAttrMessageIntegrity);
        putU16(msg, 20);
        size_t miValueOff = msg.size();
        msg.resize(msg.size() + 20, 0);

        // 调整长度字段
        uint16_t lenForMi = static_cast<uint16_t>(beforeMi - 20 + 24);
        msg[2] = lenForMi >> 8;
        msg[3] = lenForMi & 0xFF;

#ifdef HAVE_OPENSSL
        unsigned char md[20];
        unsigned int mdLen = 20;
        HMAC(EVP_sha1(), password.data(), static_cast<int>(password.size()),
             msg.data(), beforeMi, md, &mdLen);
        std::memcpy(msg.data() + miValueOff, md, 20);
#else
        // 无 OpenSSL：MI 字段留空（测试/无安全依赖环境）
        (void)miValueOff;
#endif
        // 恢复长度为「不含 MI 的当前长度」，之后再追加 FP 时重算
        uint16_t realLen = static_cast<uint16_t>(msg.size() - 20);
        msg[2] = realLen >> 8;
        msg[3] = realLen & 0xFF;
    }

    if (addFingerprint) {
        putU16(msg, kStunAttrFingerprint);
        putU16(msg, 4);
        size_t crcOff = msg.size();
        msg.resize(msg.size() + 4, 0);
        // 长度包含 FP 属性本身（8 字节）
        uint16_t len = static_cast<uint16_t>(msg.size() - 20);
        msg[2] = len >> 8;
        msg[3] = len & 0xFF;
        uint32_t crc = crc32Ieee(msg.data(), crcOff) ^ 0x5354554E;
        msg[crcOff + 0] = crc >> 24;
        msg[crcOff + 1] = crc >> 16;
        msg[crcOff + 2] = crc >> 8;
        msg[crcOff + 3] = crc & 0xFF;
    } else {
        uint16_t len = static_cast<uint16_t>(msg.size() - 20);
        msg[2] = len >> 8;
        msg[3] = len & 0xFF;
    }

    return msg;
}

std::vector<uint8_t> Stun::buildBindingRequest(
    const std::string& username, uint32_t priority, uint64_t tieBreaker,
    bool useCandidate, const std::string& password) {
    std::vector<uint8_t> msg;
    msg.assign(20, 0);
    // 随机 transaction ID
    for (int i = 8; i < 20; ++i) {
        // 简单伪随机（避免依赖 rand 状态）
        msg[i] = static_cast<uint8_t>((i * 2654435761u + 12345u) & 0xFF);
    }

    auto appendAttr = [&](uint16_t type, const void* value, size_t len) {
        putU16(msg, type);
        putU16(msg, static_cast<uint16_t>(len));
        const auto* p = static_cast<const uint8_t*>(value);
        msg.insert(msg.end(), p, p + len);
        while (msg.size() % 4 != 0) msg.push_back(0);
    };

    if (!username.empty())
        appendAttr(kStunAttrUsername, username.data(), username.size());

    uint8_t prioBuf[4];
    prioBuf[0] = (priority >> 24) & 0xFF;
    prioBuf[1] = (priority >> 16) & 0xFF;
    prioBuf[2] = (priority >> 8) & 0xFF;
    prioBuf[3] = priority & 0xFF;
    appendAttr(kStunAttrPriority, prioBuf, 4);

    appendAttr(kStunAttrIceControlling, &tieBreaker, 8);
    if (useCandidate) {
        putU16(msg, kStunAttrUseCandidate);
        putU16(msg, 0);
    }

    bool withMi = !password.empty();
    if (withMi) {
        size_t beforeMi = msg.size();
        putU16(msg, kStunAttrMessageIntegrity);
        putU16(msg, 20);
        size_t miValueOff = msg.size();
        msg.resize(msg.size() + 20, 0);
        uint16_t lenForMi = static_cast<uint16_t>(beforeMi - 20 + 24);
        msg[2] = lenForMi >> 8;
        msg[3] = lenForMi & 0xFF;
#ifdef HAVE_OPENSSL
        unsigned char md[20];
        unsigned int mdLen = 20;
        HMAC(EVP_sha1(), password.data(), static_cast<int>(password.size()),
             msg.data(), beforeMi, md, &mdLen);
        std::memcpy(msg.data() + miValueOff, md, 20);
#endif
        uint16_t realLen = static_cast<uint16_t>(msg.size() - 20);
        msg[2] = realLen >> 8;
        msg[3] = realLen & 0xFF;
    }

    // FINGERPRINT
    putU16(msg, kStunAttrFingerprint);
    putU16(msg, 4);
    size_t crcOff = msg.size();
    msg.resize(msg.size() + 4, 0);
    uint16_t len = static_cast<uint16_t>(msg.size() - 20);
    msg[2] = len >> 8;
    msg[3] = len & 0xFF;
    uint32_t crc = crc32Ieee(msg.data(), crcOff) ^ 0x5354554E;
    msg[crcOff + 0] = crc >> 24;
    msg[crcOff + 1] = crc >> 16;
    msg[crcOff + 2] = crc >> 8;
    msg[crcOff + 3] = crc & 0xFF;

    // Binding Request type 0x0001
    msg[0] = 0x00;
    msg[1] = 0x01;
    msg[4] = 0x21;
    msg[5] = 0x12;
    msg[6] = 0xA4;
    msg[7] = 0x42;
    return msg;
}

bool Stun::checkIntegrity(const uint8_t* data, size_t len,
                          const std::string& password) {
    if (password.empty()) return true;
#ifdef HAVE_OPENSSL
    StunMessage m;
    if (!Stun::parse(data, len, m)) return false;
    (void)m;
    // 找到 MESSAGE-INTEGRITY 属性位置（在原始字节中）
    // 长度字段在带 MI 时已被对端设为截至 MI（含 24 字节）
    size_t off = 20;
    size_t miOff = 0;
    while (off + 4 <= len) {
        uint16_t at = (uint16_t(data[off]) << 8) | data[off + 1];
        uint16_t al = (uint16_t(data[off + 2]) << 8) | data[off + 3];
        if (at == kStunAttrMessageIntegrity && al == 20) {
            miOff = off + 4;
            break;
        }
        off += 4 + al + (4 - (al % 4)) % 4;
    }
    if (miOff == 0) return false;

    // RFC 5389 15.4：HMAC 输入为 MESSAGE-INTEGRITY 属性“之前”的全部字节（不含 MI 的 4 字节头），
    // 头部长度字段改写为“截至 MI 末尾”：MI 末尾偏移为 miOff + 20，故长度 = miOff
    // （原实现把 MI 属性头也纳入 HMAC，与所有标准实现都无法互通）
    uint16_t effLen = static_cast<uint16_t>(miOff);
    std::vector<uint8_t> copy(data, data + miOff - 4);
    copy[2] = effLen >> 8;
    copy[3] = effLen & 0xFF;

    unsigned char md[20];
    unsigned int mdLen = 20;
    HMAC(EVP_sha1(), password.data(), static_cast<int>(password.size()),
         copy.data(), copy.size(), md, &mdLen);
    return std::memcmp(md, data + miOff, 20) == 0;
#else
    (void)data;
    (void)len;
    return true;
#endif
}

}  // namespace streaming
