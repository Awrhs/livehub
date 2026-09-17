#include "api/HttpResponse.hpp"

#include <stdio.h>

namespace streaming {

const char* HttpResponse::statusString(HttpStatusCode code) {
    switch (code) {
        case HttpStatusCode::k200Ok: return "OK";
        case HttpStatusCode::k201Created: return "Created";
        case HttpStatusCode::k204NoContent: return "No Content";
        case HttpStatusCode::k206PartialContent: return "Partial Content";
        case HttpStatusCode::k301MovedPermanently: return "Moved Permanently";
        case HttpStatusCode::k302Found: return "Found";
        case HttpStatusCode::k400BadRequest: return "Bad Request";
        case HttpStatusCode::k401Unauthorized: return "Unauthorized";
        case HttpStatusCode::k403Forbidden: return "Forbidden";
        case HttpStatusCode::k404NotFound: return "Not Found";
        case HttpStatusCode::k405MethodNotAllowed: return "Method Not Allowed";
        case HttpStatusCode::k429TooManyRequests: return "Too Many Requests";
        case HttpStatusCode::k500InternalServerError: return "Internal Server Error";
        default: return "Unknown";
    }
}

void HttpResponse::appendToBuffer(Buffer* output) const {
    char buf[64];
    int len = ::snprintf(buf, sizeof(buf), "HTTP/1.1 %d ",
                         static_cast<int>(statusCode_));
    output->append(buf, static_cast<size_t>(len));

    std::string status = statusMessage_.empty()
                             ? statusString(statusCode_)
                             : statusMessage_;
    output->append(status);

    if (closeConnection_) {
        output->append("\r\nConnection: close\r\n");
    } else {
        output->append("\r\nConnection: Keep-Alive\r\n");
    }

    for (const auto& header : headers_) {
        output->append(header.first);
        output->append(": ");
        output->append(header.second);
        output->append("\r\n");
    }

    // 字符串 body 需要设 Content-Length（文件 body 已在 setFileBody 设过）
    if (!body_.empty() && headers_.find("Content-Length") == headers_.end()) {
        output->append("Content-Length: ");
        output->append(std::to_string(body_.size()));
        output->append("\r\n");
    }

    output->append("\r\n");
    if (!body_.empty()) {
        output->append(body_);
    }
    // 文件 body 由 HttpServer 在发送完头部后用 sendfile 单独发送
}

}  // namespace streaming
