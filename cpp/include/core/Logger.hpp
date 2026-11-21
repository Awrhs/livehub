#pragma once

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <ctime>

namespace streaming {

/**
 * 日志级别
 */
enum class LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL,
    NUM_LOG_LEVELS,
};

/**
 * 请求级追踪上下文。
 *
 * 每个请求在其 Reactor 线程内同步处理完毕，因此用 thread_local 保存
 * 当前 trace id 即可让该线程上的所有 LOG_* 自动携带，无需改宏签名。
 *   - trace-id 中间件在请求开始时 setTraceId(...)
 *   - AccessLog（afterRequest）在请求结束时清空
 */
namespace logging {
/// 设置当前线程的 trace id（传空串清除）
void setTraceId(std::string id);
/// 当前线程的 trace id（可能为空）
const std::string& traceId();
}  // namespace logging

/**
 * Logger: 轻量级分级日志。
 *
 * 设计要点：
 * - 线程安全：使用原子 level，日志写入加锁
 * - 简单的流式接口（宏封装），避免 iostream 开耗
 * - 支持运行时动态调整级别
 * - 双格式：默认人类可读单行；setJsonMode(true) 后输出 JSON 行
 *   （msg 经 JSON 转义，便于 Loki/ELK 采集），STREAM_LOG_JSON=1 开启
 * - trace id：thread_local（见 logging 命名空间），非空时自动附加
 *
 * 简历亮点：自研日志系统，理解并发日志与性能权衡
 */
class Logger {
public:
    /// 获取单例
    static Logger& instance();

    /// 设置最低输出级别
    void setLevel(LogLevel level) { level_.store(static_cast<int>(level)); }
    LogLevel level() const {
        return static_cast<LogLevel>(level_.load());
    }

    /// JSON 行格式开关（访问日志等也跟随此开关）
    void setJsonMode(bool on) { jsonMode_.store(on); }
    bool jsonMode() const { return jsonMode_.load(); }

    /// 原子化写入一行日志（线程安全）
    void log(LogLevel level, const char* file, int line,
             const char* fmt, ...);

    /// 直接写一行已组装完成的日志（不添加任何前缀/trace）。
    /// 供 AccessLog 等自行组装完整 JSON 行时使用。
    void logRaw(const char* data, size_t len);

    /// JSON 字符串转义（" \ 控制字符），公共静态便于单测复用
    static std::string jsonEscape(const std::string& s);

    /// 设置输出到文件（默认 stdout）
    bool setOutputFile(const std::string& path);

private:
    Logger();

    std::atomic<int> level_;
    std::atomic<bool> jsonMode_{false};
    std::FILE* fp_;
    bool ownFp_;  // 是否拥有 fp（需关闭）

    // 线程安全写入
    void writeLine(const char* data, size_t len);
    void flush();
};

// 当前文件名（编译期）
#define LOG_BASENAME \
    (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

// 格式串并入 __VA_ARGS__（不用 ##__VA_ARGS__ 扩展），无参数调用也符合 C++17 标准
#define LOG_IMPL(level, ...)                                             \
    do {                                                                 \
        ::streaming::Logger::instance().log(level, LOG_BASENAME,         \
                                            __LINE__, __VA_ARGS__);      \
    } while (0)

#define LOG_TRACE(...) LOG_IMPL(::streaming::LogLevel::TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) LOG_IMPL(::streaming::LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  LOG_IMPL(::streaming::LogLevel::INFO,  __VA_ARGS__)
#define LOG_WARN(...)  LOG_IMPL(::streaming::LogLevel::WARN,  __VA_ARGS__)
#define LOG_ERROR(...) LOG_IMPL(::streaming::LogLevel::ERROR, __VA_ARGS__)
#define LOG_FATAL(...) LOG_IMPL(::streaming::LogLevel::FATAL, __VA_ARGS__)

/// 日志级别字符串（紧凑，无对齐填充）
const char* levelToString(LogLevel level);

}  // namespace streaming
