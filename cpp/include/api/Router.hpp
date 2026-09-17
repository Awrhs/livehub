#pragma once

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"

#include <functional>
#include <string>
#include <vector>

namespace streaming {

/**
 * HTTP 路由处理器
 * @param req 请求
 * @param resp 响应（处理器填充）
 */
using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

/**
 * Router: 路径路由分发。
 *
 * 支持：
 * - 精确路径匹配
 * - 路径参数 /api/stream/:id -> 提取 id
 * - 按 HTTP 方法分发
 *
 * 简历亮点：自研路由匹配与参数提取
 */
class Router {
public:
    void get(const std::string& path, Handler h) {
        routes_.push_back({HttpMethod::GET, path, std::move(h)});
    }
    void post(const std::string& path, Handler h) {
        routes_.push_back({HttpMethod::POST, path, std::move(h)});
    }
    void add(HttpMethod method, const std::string& path, Handler h) {
        routes_.push_back({method, path, std::move(h)});
    }

    /// 路由分发，返回是否匹配
    bool dispatch(HttpRequest& req, HttpResponse& resp);

    /// 设置 404 处理器
    void setNotFoundHandler(Handler h) { notFound_ = std::move(h); }

private:
    struct Route {
        HttpMethod method;
        std::string path;
        Handler handler;
    };

    /// 路径匹配，支持 :param 参数提取
    static bool matchPath(const std::string& pattern, const std::string& path,
                          std::map<std::string, std::string>& params);

    std::vector<Route> routes_;
    Handler notFound_;
};

}  // namespace streaming
