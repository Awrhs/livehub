#pragma once

#include "core/Buffer.hpp"

#include <functional>
#include <memory>
#include <string>
#include <sys/sendfile.h>
#include <unordered_map>

namespace streaming {

class TcpConnection;

/**
 * HTTP 响应状态码
 */
enum class HttpStatusCode {
    kUnknown = 0,
    k200Ok = 200,
    k201Created = 201,
    k204NoContent = 204,
    k206PartialContent = 206,
    k301MovedPermanently = 301,
    k302Found = 302,
    k400BadRequest = 400,
    k401Unauthorized = 401,
    k403Forbidden = 403,
    k404NotFound = 404,
    k409Conflict = 409,
    k405MethodNotAllowed = 405,
    k429TooManyRequests = 429,
    k500InternalServerError = 500,
    k502BadGateway = 502,
    k503ServiceUnavailable = 503,
};

/**
 * HttpResponse: HTTP 响应。
 *
 * 支持两种 body：
 * 1. 内存字符串（JSON 等）
 * 2. 文件（通过 sendfile 零拷贝发送，适合 TS 等大文件）
 *
 * 简历亮点：sendfile 零拷贝、HTTP Range（206 Partial Content）支持
 */
class HttpResponse {
public:
    explicit HttpResponse(bool closeConnection = false)
        : statusCode_(HttpStatusCode::k200Ok),
          closeConnection_(closeConnection),
          fileFd_(-1),
          fileSize_(0) {}

    void setStatusCode(HttpStatusCode code) { statusCode_ = code; }
    HttpStatusCode statusCode() const { return statusCode_; }
    void setStatusMessage(const std::string& msg) { statusMessage_ = msg; }

    /// 设置 Content-Type
    void setContentType(const std::string& type) {
        addHeader("Content-Type", type);
    }

    void addHeader(const std::string& key, const std::string& value) {
        headers_[key] = value;
    }

    /// 设置字符串 body
    void setBody(const std::string& body) {
        body_ = body;
    }
    const std::string& body() const { return body_; }

    /// 设置共享 body（如内存缓存中的分片）：发送时只传递引用，不拷贝
    void setSharedBody(std::shared_ptr<const std::string> body) {
        addHeader("Content-Length", std::to_string(body ? body->size() : 0));
        sharedBody_ = std::move(body);
    }
    const std::shared_ptr<const std::string>& sharedBody() const { return sharedBody_; }

    /// 设置文件（用 sendfile 发送）
    /// @param fd 已打开的文件描述符
    /// @param size 文件大小
    /// @param contentType MIME 类型
    void setFileBody(int fd, size_t size, const std::string& contentType) {
        fileFd_ = fd;
        setStatusCode(HttpStatusCode::k200Ok);
        fileSize_ = size;
        setContentType(contentType);
        addHeader("Content-Length", std::to_string(size));
        addHeader("Accept-Ranges", "bytes");
    }

    /// 设置 Range 响应（206 Partial Content）
    void setRangeBody(int fd, size_t fileSize, size_t start, size_t end,
                      const std::string& contentType) {
        fileFd_ = fd;
        rangeStart_ = start;
        rangeEnd_ = end;
        fileSize_ = fileSize;
        isRange_ = true;
        setStatusCode(HttpStatusCode::k206PartialContent);
        setStatusMessage("Partial Content");
        setContentType(contentType);
        addHeader("Content-Range",
                  "bytes " + std::to_string(start) + "-" +
                  std::to_string(end) + "/" + std::to_string(fileSize));
        addHeader("Content-Length", std::to_string(end - start + 1));
        addHeader("Accept-Ranges", "bytes");
    }

    bool closeConnection() const { return closeConnection_; }
    void setCloseConnection(bool on) { closeConnection_ = on; }

    /// 流式响应（HTTP-FLV 等长连接）：头部发出后调用此回调，处理器可持有
    /// TcpConnection 持续 push 数据；连接必须保持打开（closeConnection=false）。
    using StreamCallback =
        std::function<void(const std::shared_ptr<TcpConnection>&)>;
    void setStreamCallback(StreamCallback cb) {
        streamCallback_ = std::move(cb);
        closeConnection_ = false;
        isStreaming_ = true;
    }
    bool isStreaming() const { return isStreaming_; }
    const StreamCallback& streamCallback() const { return streamCallback_; }

    /// 延迟响应：处理器需要异步决定结果（如等待 webhook 鉴权）时设置。
    /// HttpServer 此时不发送任何内容，把连接交给回调；回调稍后在连接所属
    /// IO 线程用 HttpServer::sendResponse 发出完整响应。
    void setDeferred(StreamCallback cb) {
        deferredCallback_ = std::move(cb);
        closeConnection_ = false;
    }
    bool isDeferred() const { return static_cast<bool>(deferredCallback_); }
    const StreamCallback& deferredCallback() const { return deferredCallback_; }

    /// 把响应头部序列化到 buffer（body 字符串也一起）
    /// 序列化响应。includeSharedBody=false 时不追加共享 body（由调用方零拷贝发送）
    void appendToBuffer(Buffer* output, bool includeSharedBody = true) const;

    /// 是否用 sendfile 发送文件
    bool hasFileBody() const { return fileFd_ >= 0; }
    int fileFd() const { return fileFd_; }
    size_t fileSize() const { return fileSize_; }
    size_t rangeStart() const { return rangeStart_; }
    size_t rangeEnd() const { return rangeEnd_; }
    bool isRange() const { return isRange_; }

    static const char* statusString(HttpStatusCode code);

private:
    HttpStatusCode statusCode_;
    std::string statusMessage_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
    std::shared_ptr<const std::string> sharedBody_;
    bool closeConnection_;

    // 文件 body（sendfile）
    int fileFd_;
    size_t fileSize_;
    size_t rangeStart_ = 0;
    size_t rangeEnd_ = 0;
    bool isRange_ = false;

    // 流式响应
    bool isStreaming_ = false;
    StreamCallback streamCallback_;
    StreamCallback deferredCallback_;
};

}  // namespace streaming
