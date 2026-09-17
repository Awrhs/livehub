#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

namespace streaming {

namespace rtsp_detail {
inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}
}  // namespace rtsp_detail

/**
 * RtspRequest / RtspResponse: RTSP 文本协议（RFC 2326）。
 *
 * 请求形如：
 *   METHOD rtsp://host/path RTSP/1.0
 *   CSeq: 1
 *   Session: abc
 *   Transport: RTP/AVP/TCP;interleaved=0-1
 *   <body>
 *
 * 与 HTTP 结构相似，这里做最小解析（按 \r\n\r\n 分头/体）。
 */
struct RtspRequest {
    std::string method;
    std::string url;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;

    std::string header(const std::string& key) const {
        for (const auto& kv : headers) {
            if (rtsp_detail::iequals(kv.first, key)) return kv.second;
        }
        return "";
    }
    int cseq() const {
        std::string s = header("CSeq");
        return s.empty() ? 0 : std::atoi(s.c_str());
    }
};

struct RtspResponse {
    int statusCode = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    void setHeader(const std::string& k, const std::string& v) { headers[k] = v; }

    std::string serialize() const {
        std::ostringstream o;
        o << "RTSP/1.0 " << statusCode << " " << reason << "\r\n";
        for (const auto& kv : headers) {
            o << kv.first << ": " << kv.second << "\r\n";
        }
        if (!body.empty()) {
            o << "Content-Length: " << body.size() << "\r\n";
        }
        o << "\r\n";
        o << body;
        return o.str();
    }
};

/// 从 buffer 解析一个 RTSP 请求；返回已消费的字节数（0=数据不足）。
size_t parseRtspRequest(const char* data, size_t len, RtspRequest& out);

}  // namespace streaming
