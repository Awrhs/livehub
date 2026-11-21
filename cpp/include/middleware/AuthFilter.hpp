#pragma once

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/Jwt.hpp"
#include "auth/TokenBlacklist.hpp"
#include "core/NonCopyable.hpp"
#include "net/TcpConnection.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

namespace streaming {

/**
 * AuthFilter: JWT 鉴权中间件。
 *
 * 从 Authorization: Bearer <token> 读取 JWT，校验签名与过期；
 * 成功则把 user 注入 req.attributes["user"]，继续下一个中间件；
 * 失败返回 401。
 *
 * 注入 TokenBlacklist 后额外校验 jti：已注销（/api/logout）的 token
 * 即使签名有效也返回 401（黑名单本地 + Redis 双层，见 TokenBlacklist）。
 *
 * 白名单路径（前缀匹配，如 /api/health、/live/、/api/token、/metrics
 * 视配置）直接放行。
 */
class AuthFilter : NonCopyable {
public:
    explicit AuthFilter(std::shared_ptr<Jwt> jwt);

    /// 添加放行前缀
    void allow(const std::string& pathPrefix) { whitelist_.insert(pathPrefix); }

    /// 静态管理令牌（常量时间比较）。为空则只接受 role=admin 的 JWT。
    void setAdminToken(std::string t) { adminToken_ = std::move(t); }

    /// 注入 JWT 注销黑名单（可选；不注入则不校验注销）
    void setBlacklist(std::shared_ptr<TokenBlacklist> bl) {
        blacklist_ = std::move(bl);
    }

    /// 中间件入口
    bool operator()(const std::shared_ptr<TcpConnection>&,
                    HttpRequest& req, HttpResponse& resp);

private:
    bool isWhitelisted(const std::string& path) const;

    std::shared_ptr<Jwt> jwt_;
    std::shared_ptr<TokenBlacklist> blacklist_;
    std::unordered_set<std::string> whitelist_;
    std::string adminToken_;
};

}  // namespace streaming
