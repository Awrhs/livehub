#include "protocol/WebSocket.hpp"
#include "core/Logger.hpp"

#include <arpa/inet.h>

#include <cstring>

#ifdef HAVE_OPENSSL
#include <openssl/sha.h>
#endif

namespace streaming {

namespace {

// Base64 编码（标准，带 padding）
std::string b64(const uint8_t* data, size_t len) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) v |= uint32_t(data[i + 2]);
        out.push_back(t[(v >> 18) & 0x3F]);
        out.push_back(t[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? t[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? t[v & 0x3F] : '=');
    }
    return out;
}

#ifndef HAVE_OPENSSL
// 自包含 SHA-1（RFC 3174），用于无 OpenSSL 环境
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                     0xC3D2E1F0};
    uint8_t block[64];
    size_t blockLen = 0;
    uint64_t totalLen = 0;

    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void process(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) |
                   (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = tmp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    void update(const uint8_t* data, size_t len) {
        totalLen += len;
        while (len > 0) {
            size_t take = 64 - blockLen;
            if (take > len) take = len;
            std::memcpy(block + blockLen, data, take);
            blockLen += take;
            data += take;
            len -= take;
            if (blockLen == 64) {
                process(block);
                blockLen = 0;
            }
        }
    }
    std::vector<uint8_t> finalize() {
        uint64_t bitLen = totalLen * 8;  // 填充前的消息位长
        uint8_t pad = 0x80;
        appendRaw(&pad, 1);
        uint8_t zero = 0;
        while (blockLen != 56) appendRaw(&zero, 1);
        uint8_t lenBuf[8];
        for (int i = 0; i < 8; ++i)
            lenBuf[i] = static_cast<uint8_t>(bitLen >> (56 - i * 8));
        appendRaw(lenBuf, 8);
        std::vector<uint8_t> out(20);
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = h[i] >> 24;
            out[i * 4 + 1] = h[i] >> 16;
            out[i * 4 + 2] = h[i] >> 8;
            out[i * 4 + 3] = h[i];
        }
        return out;
    }

    // 追加填充字节但不计入 totalLen（仅驱动 block）
    void appendRaw(const uint8_t* data, size_t len) {
        while (len > 0) {
            size_t take = 64 - blockLen;
            if (take > len) take = len;
            std::memcpy(block + blockLen, data, take);
            blockLen += take;
            data += take;
            len -= take;
            if (blockLen == 64) {
                process(block);
                blockLen = 0;
            }
        }
    }
};
#endif

void sha1(const std::string& input, uint8_t out[20]) {
#ifdef HAVE_OPENSSL
    SHA1(reinterpret_cast<const uint8_t*>(input.data()), input.size(), out);
#else
    Sha1 s;
    s.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    auto digest = s.finalize();
    std::memcpy(out, digest.data(), 20);
#endif
}
}  // namespace

std::string WebSocket::acceptKey(const std::string& clientKey) {
    static const std::string guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = clientKey + guid;
    uint8_t digest[20];
    sha1(concat, digest);
    return b64(digest, 20);
}

std::vector<uint8_t> WebSocket::buildFrame(WsOpcode opcode, const uint8_t* data,
                                          size_t len, bool fin) {
    std::vector<uint8_t> f;
    f.push_back((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode));
    // 服务端不 mask，最高位为 0
    if (len < 126) {
        f.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        f.push_back(126);
        f.push_back((len >> 8) & 0xFF);
        f.push_back(len & 0xFF);
    } else {
        f.push_back(127);
        uint64_t l = len;
        for (int i = 7; i >= 0; --i) f.push_back((l >> (i * 8)) & 0xFF);
    }
    f.insert(f.end(), data, data + len);
    return f;
}

std::vector<uint8_t> WebSocket::buildClose(uint16_t code) {
    uint8_t body[2] = {static_cast<uint8_t>(code >> 8),
                       static_cast<uint8_t>(code & 0xFF)};
    return buildFrame(kWsClose, body, 2);
}

bool WebSocket::parseFrames(Buffer& buf, const FrameCallback& cb) {
    while (buf.readableBytes() >= 2) {
        const uint8_t* p =
            reinterpret_cast<const uint8_t*>(buf.peek());
        bool fin = (p[0] & 0x80) != 0;
        uint8_t opcode = p[0] & 0x0F;
        bool masked = (p[1] & 0x80) != 0;
        uint64_t payloadLen = p[1] & 0x7F;
        size_t headerLen = 2;

        if (payloadLen == 126) {
            if (buf.readableBytes() < 4) return true;
            payloadLen = (uint64_t(p[2]) << 8) | p[3];
            headerLen = 4;
        } else if (payloadLen == 127) {
            if (buf.readableBytes() < 10) return true;
            payloadLen = 0;
            for (int i = 0; i < 8; ++i)
                payloadLen = (payloadLen << 8) | p[2 + i];
            headerLen = 10;
        }
        uint8_t maskKey[4] = {0};
        if (masked) {
            if (buf.readableBytes() < headerLen + 4) return true;
            std::memcpy(maskKey, p + headerLen, 4);
            headerLen += 4;
        }
        if (buf.readableBytes() < headerLen + payloadLen) return true;

        std::vector<uint8_t> payload(payloadLen);
        const uint8_t* d = p + headerLen;
        for (uint64_t i = 0; i < payloadLen; ++i) {
            payload[i] = masked ? d[i] ^ maskKey[i % 4] : d[i];
        }
        buf.retrieve(headerLen + static_cast<size_t>(payloadLen));

        if (opcode == kWsPing) {
            (void)fin;
            // pong 由上层发送
            if (cb) cb(kWsPong, payload.data(), payload.size());
        } else {
            if (cb) cb(static_cast<WsOpcode>(opcode), payload.data(),
                       payload.size());
        }
        // close
        if (opcode == kWsClose) return false;
    }
    return true;
}

}  // namespace streaming
