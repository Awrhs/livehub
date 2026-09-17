#include "middleware/AuthFilter.hpp"

#include "api/HttpResponse.hpp"
#include "core/Logger.hpp"

#include <strings.h>

namespace streaming {

AuthFilter::AuthFilter(std::shared_ptr<Jwt> jwt)
    : jwt_(std::move(jwt)) {
    // 默认放行
    allow("/api/health");
    allow("/api/token");
    allow("/live/");
}

bool AuthFilter::isWhitelisted(const std::string& path) const {
    for (const auto& prefix : whitelist_) {
        if (path.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

bool AuthFilter::operator()(
    const std::shared_ptr<TcpConnection>&, HttpRequest& req,
    HttpResponse& resp) {
    const std::string& path = req.path();
    if (isWhitelisted(path)) return true;

    std::string auth = req.header("Authorization");
    if (auth.compare(0, 7, "Bearer ") == 0) {
        std::string token = auth.substr(7);
        if (jwt_) {
            auto payload = jwt_->verify(token);
            if (!payload.empty()) {
                auto it = payload.find("sub");
                if (it != payload.end()) {
                    req.setAttribute("user", it->second);
                }
                auto jti = payload.find("jti");
                if (jti != payload.end()) {
                    // 注销校验：签名有效但已 logout 的 token 拒绝
                    if (blacklist_ && blacklist_->contains(jti->second)) {
                        resp.setStatusCode(
                            HttpStatusCode::k401Unauthorized);
                        resp.setContentType("application/json");
                        resp.setBody(
                            "{\"error\":\"token revoked\"}");
                        return false;
                    }
                    req.setAttribute("jti", jti->second);
                }
                return true;
            }
        }
    }

    resp.setStatusCode(HttpStatusCode::k401Unauthorized);
    resp.setContentType("application/json");
    resp.setBody("{\"error\":\"unauthorized\"}");
    return false;
}

}  // namespace streaming
