#include "api/Jwt.hpp"
#include "core/Logger.hpp"

#include <chrono>
#include <cstring>
#include <sstream>

#ifdef HAVE_OPENSSL
#include <openssl/hmac.h>
#include <openssl/sha.h>
#endif

namespace streaming {

// ---- Base64URL ----

static const char kBase64UrlTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string Jwt::base64UrlEncode(const std::string& data) {
    size_t inLen = data.size();
    size_t outLen = 4 * ((inLen + 2) / 3);
    std::string out;
    out.reserve(outLen);

    for (size_t i = 0; i < inLen; i += 3) {
        uint32_t v = static_cast<uint8_t>(data[i]) << 16;
        if (i + 1 < inLen) v |= static_cast<uint8_t>(data[i + 1]) << 8;
        if (i + 2 < inLen) v |= static_cast<uint8_t>(data[i + 2]);

        out.push_back(kBase64UrlTable[(v >> 18) & 0x3F]);
        out.push_back(kBase64UrlTable[(v >> 12) & 0x3F]);
        if (i + 1 < inLen)
            out.push_back(kBase64UrlTable[(v >> 6) & 0x3F]);
        if (i + 2 < inLen)
            out.push_back(kBase64UrlTable[v & 0x3F]);
    }
    // URL-safe: 不加 '=' padding
    return out;
}

static inline int base64UrlVal(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

std::string Jwt::base64UrlDecode(const std::string& in) {
    std::string out;
    uint32_t val = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int v = base64UrlVal(c);
        if (v < 0) continue;
        val = (val << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
        }
    }
    return out;
}

// ---- HMAC-SHA256 ----

std::string Jwt::hmacSha256(const std::string& key, const std::string& data) {
    unsigned char md[32];
    unsigned int mdLen = 0;

#ifdef HAVE_OPENSSL
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         md, &mdLen);
#else
    // 无 OpenSSL 时使用简易 HMAC（非安全，仅供编译通过）
    // 真实环境应确保 OpenSSL 可用
    LOG_WARN("OpenSSL unavailable: using insecure stub HMAC");
    for (size_t i = 0; i < 32; ++i) {
        md[i] = static_cast<unsigned char>(
            (i < key.size() ? key[i] : 0) ^
            (i < data.size() ? data[i] : 0));
    }
    mdLen = 32;
#endif
    return std::string(reinterpret_cast<const char*>(md), mdLen);
}

// ---- 简易 JSON payload 构建（{ "k":"v", ... }）----

static std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

static std::string buildPayloadJson(const std::map<std::string, std::string>& payload,
                                    int64_t ttl) {
    auto now = std::chrono::system_clock::now();
    auto exp = std::chrono::duration_cast<std::chrono::seconds>(
                   now.time_since_epoch()).count() + ttl;

    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& kv : payload) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << jsonEscape(kv.first) << "\":\""
           << jsonEscape(kv.second) << "\"";
    }
    if (!first) ss << ",";
    ss << "\"exp\":" << exp << "}";
    return ss.str();
}

// ---- sign / verify ----

std::string Jwt::sign(const std::map<std::string, std::string>& payload,
                      int64_t ttlSeconds) {
    std::string header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    std::string payloadJson = buildPayloadJson(payload, ttlSeconds);

    std::string h = base64UrlEncode(header);
    std::string p = base64UrlEncode(payloadJson);
    std::string signingInput = h + "." + p;
    std::string sig = hmacSha256(secret_, signingInput);

    return signingInput + "." + base64UrlEncode(sig);
}

// 极简 payload 解析（仅提取 "key":"value" 和 "exp":number）
static void parsePayload(const std::string& json,
                         std::map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos < json.size()) {
        size_t kq1 = json.find('"', pos);
        if (kq1 == std::string::npos) break;
        size_t kq2 = json.find('"', kq1 + 1);
        if (kq2 == std::string::npos) break;
        std::string key = json.substr(kq1 + 1, kq2 - kq1 - 1);

        size_t colon = json.find(':', kq2);
        if (colon == std::string::npos) break;
        size_t vp = colon + 1;
        while (vp < json.size() && (json[vp] == ' ' || json[vp] == '\t')) ++vp;

        if (vp < json.size() && json[vp] == '"') {
            size_t vq = json.find('"', vp + 1);
            if (vq == std::string::npos) break;
            out[key] = json.substr(vp + 1, vq - vp - 1);
            pos = vq + 1;
        } else {
            // number / bool
            size_t ve = json.find_first_of(",}", vp);
            if (ve == std::string::npos) ve = json.size();
            out[key] = json.substr(vp, ve - vp);
            pos = ve;
        }
    }
}

std::map<std::string, std::string> Jwt::decodePayload(const std::string& token) {
    std::map<std::string, std::string> result;
    size_t dot1 = token.find('.');
    if (dot1 == std::string::npos) return result;
    size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return result;

    std::string payloadB64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string payloadJson = base64UrlDecode(payloadB64);
    parsePayload(payloadJson, result);
    return result;
}

std::map<std::string, std::string> Jwt::verify(const std::string& token) {
    size_t dot1 = token.find('.');
    size_t dot2 = token.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos) {
        return {};
    }

    std::string signingInput = token.substr(0, dot2);
    std::string sigB64 = token.substr(dot2 + 1);

    // 重新计算签名对比
    std::string expectedSig = base64UrlEncode(hmacSha256(secret_, signingInput));
    if (sigB64 != expectedSig) {
        return {};  // 签名不匹配
    }

    auto payload = decodePayload(token);

    // 检查过期
    auto it = payload.find("exp");
    if (it != payload.end()) {
        int64_t exp = std::stoll(it->second);
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        if (now > exp) {
            return {};
        }
    }
    return payload;
}

}  // namespace streaming
