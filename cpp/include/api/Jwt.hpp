#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace streaming {

/**
 * Jwt: HS256 JWT 签发与校验。
 *
 * 设计要点：
 * - Header.Payload.Signature 三段 Base64URL
 * - 签名用 HMAC-SHA256（OpenSSL；无 OpenSSL 时用简易 HMAC 降级）
 * - 校验签名 + 过期时间
 *
 * 简历亮点：手写 JWT，理解 Base64URL 与 HMAC 签名流程
 */
class Jwt {
public:
    explicit Jwt(std::string secret) : secret_(std::move(secret)) {}

    /// 签发 token
    /// @param payload 业务声明（如 stream_id, client_ip）
    /// @param ttlSeconds 有效期（秒）
    std::string sign(const std::map<std::string, std::string>& payload,
                     int64_t ttlSeconds = 3600);

    /// 校验 token
    /// @return 有效返回 payload，无效返回空 map
    std::map<std::string, std::string> verify(const std::string& token);

    /// 拆分 token 并取 payload（不校验签名，仅供调试）
    static std::map<std::string, std::string> decodePayload(const std::string& token);

private:
    std::string secret_;

    static std::string base64UrlEncode(const std::string& data);
    static std::string base64UrlDecode(const std::string& data);
    static std::string hmacSha256(const std::string& key, const std::string& data);
};

}  // namespace streaming
