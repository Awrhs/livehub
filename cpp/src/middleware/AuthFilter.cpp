#include "middleware/AuthFilter.hpp"

#include "api/HttpResponse.hpp"
#include "core/Logger.hpp"

#include <strings.h>

namespace streaming {

namespace {
bool constantTimeEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i)
        d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

bool deny(HttpResponse& resp, HttpStatusCode code, const char* msg) {
    resp.setStatusCode(code);
    resp.setContentType("application/json");
    resp.setBody(std::string("{\"error\":\"") + msg + "\"}");
    return false;
}
}  // namespace

AuthFilter::AuthFilter(std::shared_ptr<Jwt> jwt)
    : jwt_(std::move(jwt)) {
    // 默认放行（注意：/api/token 不再放行——此前任何人都能领取有效 token）
    allow("/api/health");
    allow("/live/");
}

bool AuthFilter::isWhitelisted(const std::string& path) const {
    for (const auto& prefix : whitelist_) {
        if (path.compare(0, prefix.size(), prefix) != 0) continue;
        // 前缀须落在路径段边界上（/api/health 不应放行 /api/healthx）
        if (path.size() == prefix.size() || prefix.back() == '/' ||
            path[prefix.size()] == '/' || path[prefix.size()] == '?')
            return true;
    }
    return false;
}

bool AuthFilter::operator()(
    const std::shared_ptr<TcpConnection>&, HttpRequest& req,
    HttpResponse& resp) {
    const std::string& path = req.path();
    if (isWhitelisted(path)) return true;

    std::string auth = req.header("Authorization");
    if (auth.compare(0, 7, "Bearer ") != 0)
        return deny(resp, HttpStatusCode::k401Unauthorized, "unauthorized");
    std::string token = auth.substr(7);

    // 1) 静态管理令牌
    if (!adminToken_.empty() && constantTimeEq(token, adminToken_)) {
        req.setAttribute("user", "admin");
        req.setAttribute("role", "admin");
        return true;
    }
    // 2) role=admin 的 JWT（流 token 不能访问管理接口）
    if (jwt_) {
        auto payload = jwt_->verify(token);
        if (!payload.empty()) {
            auto role = payload.find("role");
            if (role == payload.end() || role->second != "admin")
                return deny(resp, HttpStatusCode::k403Forbidden, "admin role required");
            auto jti = payload.find("jti");
            if (jti != payload.end()) {
                if (blacklist_ && blacklist_->contains(jti->second))
                    return deny(resp, HttpStatusCode::k401Unauthorized, "token revoked");
                req.setAttribute("jti", jti->second);
            }
            auto sub = payload.find("sub");
            req.setAttribute("user", sub != payload.end() ? sub->second : "admin");
            req.setAttribute("role", "admin");
            return true;
        }
    }
    return deny(resp, HttpStatusCode::k401Unauthorized, "unauthorized");
}

}  // namespace streaming
