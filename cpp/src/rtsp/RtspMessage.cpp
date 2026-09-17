#include "rtsp/RtspMessage.hpp"

#include <algorithm>

namespace streaming {

size_t parseRtspRequest(const char* data, size_t len, RtspRequest& out) {
    // 找头/体分隔
    const char* hdrEnd = nullptr;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' &&
            data[i + 3] == '\n') {
            hdrEnd = data + i;
            break;
        }
    }
    if (!hdrEnd) return 0;  // 头部未到齐

    std::string headerText(data, hdrEnd);
    size_t hdrBytes = static_cast<size_t>(hdrEnd - data) + 4;

    size_t contentLength = 0;
    // 解析行
    size_t pos = 0;
    bool firstLine = true;
    while (pos < headerText.size()) {
        size_t eol = headerText.find("\r\n", pos);
        if (eol == std::string::npos) eol = headerText.size();
        std::string line = headerText.substr(pos, eol - pos);
        pos = eol + 2;
        if (line.empty()) break;

        if (firstLine) {
            size_t s1 = line.find(' ');
            size_t s2 = line.find(' ', s1 + 1);
            if (s1 != std::string::npos) {
                out.method = line.substr(0, s1);
                if (s2 != std::string::npos) {
                    out.url = line.substr(s1 + 1, s2 - s1 - 1);
                    out.version = line.substr(s2 + 1);
                } else {
                    out.url = line.substr(s1 + 1);
                }
            }
            firstLine = false;
        } else {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                // trim
                size_t a = v.find_first_not_of(" \t");
                size_t b = v.find_last_not_of(" \t");
                if (a != std::string::npos) v = v.substr(a, b - a + 1);
                out.headers[k] = v;
                if (rtsp_detail::iequals(k, "Content-Length")) {
                    contentLength = static_cast<size_t>(std::atoi(v.c_str()));
                }
            }
        }
    }

    if (len < hdrBytes + contentLength) return 0;  // body 未到齐
    out.body.assign(data + hdrBytes, contentLength);
    return hdrBytes + contentLength;
}

}  // namespace streaming
