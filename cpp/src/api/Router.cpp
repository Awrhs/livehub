#include "api/Router.hpp"
#include "api/HttpResponse.hpp"

#include <sstream>

namespace streaming {

bool Router::matchPath(const std::string& pattern, const std::string& path,
                       std::map<std::string, std::string>& params) {
    // 按 '/' 切分比较
    std::vector<std::string> patParts, pathParts;
    std::istringstream pss(pattern), hss(path);
    std::string seg;
    while (std::getline(pss, seg, '/')) if (!seg.empty()) patParts.push_back(seg);
    while (std::getline(hss, seg, '/')) if (!seg.empty()) pathParts.push_back(seg);

    if (patParts.size() != pathParts.size()) return false;

    for (size_t i = 0; i < patParts.size(); ++i) {
        if (!patParts[i].empty() && patParts[i][0] == ':') {
            // 参数段
            params[patParts[i].substr(1)] = pathParts[i];
        } else if (patParts[i] == "*") {
            // 通配符（匹配剩余）
            return true;
        } else if (patParts[i] != pathParts[i]) {
            return false;
        }
    }
    return true;
}

bool Router::dispatch(HttpRequest& req, HttpResponse& resp) {
    for (const auto& route : routes_) {
        if (route.method != req.method()) continue;
        std::map<std::string, std::string> params;
        if (matchPath(route.path, req.path(), params)) {
            for (const auto& kv : params) {
                req.params_[kv.first] = kv.second;
            }
            route.handler(req, resp);
            return true;
        }
    }

    if (notFound_) {
        notFound_(req, resp);
        return true;
    }

    resp.setStatusCode(HttpStatusCode::k404NotFound);
    resp.setStatusMessage("Not Found");
    resp.setContentType("application/json");
    resp.setBody("{\"error\":\"not found\"}");
    return false;
}

}  // namespace streaming
