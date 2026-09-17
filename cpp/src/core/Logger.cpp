#include "core/Logger.hpp"
#include "core/Timestamp.hpp"

#include <cstdarg>
#include <mutex>

namespace streaming {

// ---- thread-local trace id ----

namespace logging {
std::string& traceIdRef() {
    // 函数内 thread_local：避免静态初始化顺序问题，且每线程独立析构
    static thread_local std::string id;
    return id;
}
void setTraceId(std::string id) { traceIdRef() = std::move(id); }
const std::string& traceId() { return traceIdRef(); }
}  // namespace logging

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger()
    : level_(static_cast<int>(LogLevel::INFO)),
      fp_(stdout),
      ownFp_(false) {}

const char* levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default:              return "?????";
    }
}

// 日志写入受一个全局 mutex 保护，避免多线程输出交错
static std::mutex& logMutex() {
    static std::mutex m;
    return m;
}

void Logger::writeLine(const char* data, size_t len) {
    if (fp_) {
        std::fwrite(data, 1, len, fp_);
        std::fputc('\n', fp_);
    }
}

void Logger::flush() {
    if (fp_) std::fflush(fp_);
}

bool Logger::setOutputFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(logMutex());
    if (ownFp_ && fp_) {
        std::fclose(fp_);
    }
    fp_ = std::fopen(path.c_str(), "ae");  // 'e' 设置 O_CLOEXEC
    if (!fp_) {
        fp_ = stdout;
        ownFp_ = false;
        return false;
    }
    ownFp_ = true;
    return true;
}

std::string Logger::jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    static const char* kHex = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xf];
                    out += kHex[c & 0xf];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void Logger::logRaw(const char* data, size_t len) {
    std::lock_guard<std::mutex> lock(logMutex());
    writeLine(data, len);
}

void Logger::log(LogLevel level, const char* file, int line,
                 const char* fmt, ...) {
    if (static_cast<int>(level) < level_.load()) {
        return;
    }

    // 格式化时间戳
    Timestamp ts = Timestamp::now();
    std::string timeStr = ts.toFormattedString(true);

    // 格式化用户消息
    char msgBuf[2048];
    va_list args;
    va_start(args, fmt);
    int msgLen = std::vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);
    if (msgLen < 0) msgLen = 0;
    if (msgLen >= static_cast<int>(sizeof(msgBuf))) {
        msgLen = sizeof(msgBuf) - 1;
    }
    msgBuf[msgLen] = '\0';

    const std::string& tid = logging::traceId();

    if (jsonMode_.load()) {
        // JSON 行：{"ts":..,"level":..,"file":..,"line":..,"trace_id":..,"msg":..}
        std::string jsonLine;
        jsonLine.reserve(static_cast<size_t>(msgLen) + timeStr.size() + 96);
        jsonLine += "{\"ts\":\"";
        jsonLine += timeStr;
        jsonLine += "\",\"level\":\"";
        jsonLine += levelToString(level);
        jsonLine += "\",\"file\":\"";
        jsonLine += jsonEscape(file);
        jsonLine += "\",\"line\":";
        jsonLine += std::to_string(line);  // 函数参数（行号）
        jsonLine += ",\"trace_id\":\"";
        jsonLine += jsonEscape(tid);
        jsonLine += "\",\"msg\":\"";
        jsonLine += jsonEscape(msgBuf);
        jsonLine += "\"}";
        std::lock_guard<std::mutex> lock(logMutex());
        writeLine(jsonLine.data(), jsonLine.size());
        if (level == LogLevel::FATAL) {
            flush();
            std::abort();
        }
        return;
    }

    // 人类可读单行；trace id 非空时追加在行尾
    char lineBuf[4096 + 64];
    int len = std::snprintf(lineBuf, sizeof(lineBuf),
                            "%s %-5s [%s:%d] ",
                            timeStr.c_str(),
                            levelToString(level),
                            file, line);
    if (len < 0) return;

    // 拼接消息
    size_t total = static_cast<size_t>(len);
    if (total + static_cast<size_t>(msgLen) < sizeof(lineBuf)) {
        std::memcpy(lineBuf + total, msgBuf, static_cast<size_t>(msgLen));
        total += static_cast<size_t>(msgLen);
    }

    // [trace=xxx]（thread-local，见 logging 命名空间）
    if (!tid.empty() &&
        total + tid.size() + 10 < sizeof(lineBuf)) {
        int t = std::snprintf(lineBuf + total,
                              sizeof(lineBuf) - total, " [trace=%s]",
                              tid.c_str());
        if (t > 0) total += static_cast<size_t>(t);
    }

    {
        std::lock_guard<std::mutex> lock(logMutex());
        writeLine(lineBuf, total);
        // FATAL 立即刷新并 abort
        if (level == LogLevel::FATAL) {
            flush();
            std::abort();
        }
    }
}

// Timestamp 格式化实现
std::string Timestamp::toFormattedString(bool showMicroseconds) const {
    char buf[64];
    time_t seconds = static_cast<time_t>(secondsSinceEpoch());
    struct tm tm_time;
    ::localtime_r(&seconds, &tm_time);

    if (showMicroseconds) {
        int microseconds = static_cast<int>(
            microSecondsSinceEpoch_ % kMicroSecondsPerSecond);
        std::snprintf(buf, sizeof(buf),
                      "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                      tm_time.tm_year + 1900, tm_time.tm_mon + 1,
                      tm_time.tm_mday, tm_time.tm_hour, tm_time.tm_min,
                      tm_time.tm_sec, microseconds);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "%04d-%02d-%02d %02d:%02d:%02d",
                      tm_time.tm_year + 1900, tm_time.tm_mon + 1,
                      tm_time.tm_mday, tm_time.tm_hour, tm_time.tm_min,
                      tm_time.tm_sec);
    }
    return std::string(buf);
}

}  // namespace streaming
