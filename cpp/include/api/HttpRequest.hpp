#pragma once

#include "core/Buffer.hpp"

#include <map>
#include <string>
#include <string_view>

namespace streaming {

/**
 * HTTP 请求方法
 */
enum class HttpMethod {
    INVALID,
    GET,
    POST,
    PUT,
    DELETE,
    HEAD,
    OPTIONS,
};

/**
 * HTTP 请求解析状态机
 */
enum class HttpRequestParseState {
    kParseRequestLine,  // 解析请求行 METHOD PATH VERSION
    kParseHeaders,      // 解析头部
    kParseBody,         // 解析 body
    kParseDone,
    kParseError,        // 请求非法（格式错误/超限），连接应回错误码并关闭
};

/**
 * HttpRequest: HTTP/1.1 请求。
 *
 * 解析采用状态机：先请求行，再头部，最后 body。
 * 支持分多次到达的数据（buffer 可能不完整）。
 *
 * 简历亮点：手写 HTTP 协议解析状态机
 */
class HttpRequest {
public:
    HttpRequest() : method_(HttpMethod::INVALID), state_(HttpRequestParseState::kParseRequestLine) {}

    HttpMethod method() const { return method_; }
    const std::string& path() const { return path_; }
    const std::string& query() const { return query_; }
    const std::string& version() const { return version_; }
    const std::map<std::string, std::string>& headers() const { return headers_; }

    std::string header(const std::string& key) const {
        auto it = headers_.find(toLower(key));
        if (it != headers_.end()) return it->second;
        return "";
    }

    const std::string& body() const { return body_; }

    /// 解析 buffer，返回 true 表示完整请求已解析完成
    bool parse(Buffer* buf);

    bool done() const { return state_ == HttpRequestParseState::kParseDone; }
    bool hasError() const { return state_ == HttpRequestParseState::kParseError; }
    /// 出错时建议返回的 HTTP 状态码（400 / 413 / 431 / 501）
    int errorStatus() const { return errorStatus_; }

    /// 安全上限：请求行+头部总字节、body 字节
    static constexpr size_t kMaxHeaderBytes = 64 * 1024;
    static constexpr size_t kMaxBodyBytes = 16 * 1024 * 1024;

    /// 长连接复用前必须完整清空，避免上一请求的参数/属性泄漏到下一请求
    void reset() {
        method_ = HttpMethod::INVALID;
        path_.clear();
        query_.clear();
        version_.clear();
        headers_.clear();
        params_.clear();
        body_.clear();
        attributes.clear();
        clientIp.clear();
        contentLength_ = 0;
        headerBytes_ = 0;
        errorStatus_ = 0;
        state_ = HttpRequestParseState::kParseRequestLine;
    }

    /// 查询参数
    std::string param(const std::string& key) const {
        auto it = params_.find(key);
        return it != params_.end() ? it->second : "";
    }

    /// 客户端 IP（由 HttpServer 连接层填充）
    std::string clientIp;

    /// 中间件注入的请求属性（如 user、auth 结果）
    std::map<std::string, std::string> attributes;
    void setAttribute(const std::string& k, const std::string& v) {
        attributes[k] = v;
    }
    std::string attribute(const std::string& k,
                           const std::string& def = "") const {
        auto it = attributes.find(k);
        return it != attributes.end() ? it->second : def;
    }

private:
    bool parseRequestLine(const std::string& line);
    bool parseHeader(const std::string& line);
    static std::string toLower(std::string s) {
        for (auto& c : s) c = ::tolower(c);
        return s;
    }
    void parseQueryParams();

    HttpMethod method_;
    std::string path_;
    std::string query_;       // path?query 中的 query
    std::string version_;
    std::map<std::string, std::string> headers_;
    std::map<std::string, std::string> params_;
    std::string body_;
    HttpRequestParseState state_;
    size_t contentLength_ = 0;
    size_t headerBytes_ = 0;
    int errorStatus_ = 0;
    bool fail(int status) {
        errorStatus_ = status;
        state_ = HttpRequestParseState::kParseError;
        return false;
    }

    friend class Router;
};

}  // namespace streaming
