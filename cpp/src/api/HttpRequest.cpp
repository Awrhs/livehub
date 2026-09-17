#include "api/HttpRequest.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <sstream>

namespace streaming {

bool HttpRequest::parse(Buffer* buf) {
    while (state_ != HttpRequestParseState::kParseDone) {
        if (state_ == HttpRequestParseState::kParseRequestLine ||
            state_ == HttpRequestParseState::kParseHeaders) {
            // 找一行（以 \r\n 结尾）
            const char* crlf = buf->findCRLF();
            if (!crlf) {
                return false;  // 数据不完整
            }
            std::string line(buf->peek(),
                             static_cast<size_t>(crlf - buf->peek()));
            buf->retrieveUntil(crlf + 2);  // 跳过 \r\n

            if (state_ == HttpRequestParseState::kParseRequestLine) {
                if (!parseRequestLine(line)) {
                    return false;
                }
                state_ = HttpRequestParseState::kParseHeaders;
            } else {
                // 空行表示头部结束
                if (line.empty()) {
                    // 若有 Content-Length，继续解析 body
                    std::string cl = header("content-length");
                    if (!cl.empty()) {
                        contentLength_ = std::stoul(cl);
                        state_ = HttpRequestParseState::kParseBody;
                    } else {
                        state_ = HttpRequestParseState::kParseDone;
                    }
                } else {
                    if (!parseHeader(line)) {
                        return false;
                    }
                }
            }
        } else if (state_ == HttpRequestParseState::kParseBody) {
            if (buf->readableBytes() >= contentLength_) {
                body_ = std::string(buf->peek(), contentLength_);
                buf->retrieve(contentLength_);
                state_ = HttpRequestParseState::kParseDone;
            } else {
                return false;  // body 不完整
            }
        }
    }
    return true;
}

bool HttpRequest::parseRequestLine(const std::string& line) {
    // METHOD PATH?QUERY VERSION
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;

    std::string methodStr = line.substr(0, sp1);
    std::string url = line.substr(sp1 + 1, sp2 - sp1 - 1);
    version_ = line.substr(sp2 + 1);

    if (methodStr == "GET") method_ = HttpMethod::GET;
    else if (methodStr == "POST") method_ = HttpMethod::POST;
    else if (methodStr == "PUT") method_ = HttpMethod::PUT;
    else if (methodStr == "DELETE") method_ = HttpMethod::DELETE;
    else if (methodStr == "HEAD") method_ = HttpMethod::HEAD;
    else if (methodStr == "OPTIONS") method_ = HttpMethod::OPTIONS;
    else {
        method_ = HttpMethod::INVALID;
        return false;
    }

    // 分离 path 和 query
    size_t q = url.find('?');
    if (q != std::string::npos) {
        path_ = url.substr(0, q);
        query_ = url.substr(q + 1);
    } else {
        path_ = url;
    }

    parseQueryParams();
    return true;
}

bool HttpRequest::parseHeader(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    std::string key = toLower(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    // 去除前导空格
    size_t start = value.find_first_not_of(' ');
    if (start != std::string::npos) value = value.substr(start);
    headers_[key] = value;
    return true;
}

void HttpRequest::parseQueryParams() {
    if (query_.empty()) return;
    std::istringstream ss(query_);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            params_[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
    }
}

}  // namespace streaming
