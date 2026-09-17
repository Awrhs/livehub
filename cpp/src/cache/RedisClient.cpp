#include "cache/RedisClient.hpp"

#include "core/Logger.hpp"
#include "core/Timestamp.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace streaming {

namespace {

int64_t nowMs() {
    return Timestamp::now().microSecondsSinceEpoch() / 1000;
}

/// 从 from 开始找 "\r\n"，返回 '\r' 的下标；未找到返回 std::string::npos
size_t findCrlf(const char* data, size_t len, size_t from) {
    for (size_t i = from; i + 1 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    }
    return std::string::npos;
}

/// 解析回复头部的整数（$N / *N / :N 的 N 部分）
bool parseHeaderInt(const char* data, size_t crlfPos, int64_t* out) {
    try {
        *out = std::stoll(std::string(data + 1, crlfPos - 1));
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

RedisClient& RedisClient::instance() {
    static RedisClient c;
    return c;
}

RedisClient::~RedisClient() {
    running_.store(false);
    qCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

// ---- 协议层（纯函数）----

std::string RedisClient::encodeCommand(
    const std::vector<std::string>& args) {
    std::string out;
    out.reserve(32);
    out += '*';
    out += std::to_string(args.size());
    out += "\r\n";
    for (const auto& a : args) {
        out += '$';
        out += std::to_string(a.size());
        out += "\r\n";
        out += a;
        out += "\r\n";
    }
    return out;
}

int RedisClient::parseReply(const char* data, size_t len, RedisReply* out) {
    if (!out) return -1;
    if (len < 1) return 0;

    switch (data[0]) {
        case '+':
        case '-': {
            // 简单串 / 错误：<prefix><payload>\r\n
            size_t crlf = findCrlf(data, len, 1);
            if (crlf == std::string::npos) return 0;
            out->type = (data[0] == '+') ? RedisReply::kSimple
                                         : RedisReply::kError;
            out->str.assign(data + 1, crlf - 1);
            return static_cast<int>(crlf + 2);
        }
        case ':': {
            // 整数：:<num>\r\n
            size_t crlf = findCrlf(data, len, 1);
            if (crlf == std::string::npos) return 0;
            int64_t v = 0;
            if (!parseHeaderInt(data, crlf, &v)) return -1;
            out->type = RedisReply::kInt;
            out->integer = v;
            return static_cast<int>(crlf + 2);
        }
        case '$': {
            // 批量串：$<len>\r\n<payload>\r\n；$-1 = nil
            size_t crlf = findCrlf(data, len, 1);
            if (crlf == std::string::npos) return 0;
            int64_t n = 0;
            if (!parseHeaderInt(data, crlf, &n)) return -1;
            if (n < 0) {
                out->type = RedisReply::kNil;
                return static_cast<int>(crlf + 2);
            }
            size_t bodyStart = crlf + 2;
            if (len < bodyStart + static_cast<size_t>(n) + 2) return 0;
            out->type = RedisReply::kBulk;
            out->str.assign(data + bodyStart, static_cast<size_t>(n));
            return static_cast<int>(bodyStart + static_cast<size_t>(n) + 2);
        }
        case '*': {
            // 数组：*<count>\r\n<reply>*count；*-1 = nil
            size_t crlf = findCrlf(data, len, 1);
            if (crlf == std::string::npos) return 0;
            int64_t n = 0;
            if (!parseHeaderInt(data, crlf, &n)) return -1;
            if (n < 0) {
                out->type = RedisReply::kNil;
                return static_cast<int>(crlf + 2);
            }
            out->type = RedisReply::kArray;
            out->arr.clear();
            size_t consumed = crlf + 2;
            for (int64_t i = 0; i < n; ++i) {
                RedisReply elem;
                int used = parseReply(data + consumed, len - consumed,
                                      &elem);
                if (used <= 0) return used;  // 不完整(0)或协议错误(-1)透传
                out->arr.push_back(std::move(elem));
                consumed += static_cast<size_t>(used);
            }
            return static_cast<int>(consumed);
        }
        default:
            return -1;  // 未知类型前缀
    }
}

// ---- 配置与生命周期 ----

void RedisClient::configure(const std::string& host, uint16_t port,
                            int timeoutMs, const std::string& password) {
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        host_ = host;
        port_ = port;
        timeoutMs_ = timeoutMs > 0 ? timeoutMs : 50;
        password_ = password;
    }
    if (host.empty()) return;  // 禁用：不启动线程
    if (running_.exchange(true)) {
        // 已在运行：worker 下次重连读新端点（SIGHUP 热改）
        return;
    }
    worker_ = std::thread([this] { workerLoop(); });
}

bool RedisClient::enabled() const {
    std::lock_guard<std::mutex> lk(cfgMu_);
    return !host_.empty() && running_.load();
}

// ---- 调用线程侧 ----

bool RedisClient::command(const std::vector<std::string>& args,
                          RedisReply* out) {
    if (!enabled()) return false;
    int timeoutMs = 50;
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        timeoutMs = timeoutMs_;
    }
    auto pr = std::make_shared<std::promise<RedisReply>>();
    auto fut = pr->get_future();
    {
        std::lock_guard<std::mutex> lk(qMu_);
        q_.push_back(Request{encodeCommand(args), pr});
    }
    qCv_.notify_one();
    // 等待上限 = 命令超时 + 余量（worker 的 SO_RCVTIMEO 已限制单次 IO）
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs + 200)) !=
        std::future_status::ready) {
        return false;  // 超时：调用方降级（fail-open）
    }
    RedisReply r = fut.get();
    if (r.isError()) return false;
    if (out) *out = std::move(r);
    return true;
}

bool RedisClient::incrWithTtl(const std::string& key, int windowSec,
                              int64_t* count) {
    RedisReply r;
    if (!command({"INCR", key}, &r) || r.type != RedisReply::kInt) {
        return false;
    }
    if (count) *count = r.integer;
    if (r.integer == 1) {
        // 窗口内第一次计数：设置过期，使窗口自然滚动。
        // EXPIRE 失败不致命（key 残留会在下个窗口被覆盖语义修复）。
        RedisReply exp;
        command({"EXPIRE", key, std::to_string(windowSec)}, &exp);
    }
    return true;
}

bool RedisClient::setWithTtl(const std::string& key, const std::string& val,
                             int ttlSec) {
    RedisReply r;
    return command({"SET", key, val, "EX", std::to_string(ttlSec)}, &r);
}

void RedisClient::setWithTtlAsync(const std::string& key,
                                  const std::string& val, int ttlSec) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk(qMu_);
    // 队列积压保护：异步写可丢弃（缓存回填本来就是尽力而为）
    if (q_.size() > 1024) return;
    q_.push_back(Request{
        encodeCommand({"SET", key, val, "EX", std::to_string(ttlSec)}),
        nullptr});
    qCv_.notify_one();
}

bool RedisClient::get(const std::string& key, std::string* val) {
    RedisReply r;
    if (!command({"GET", key}, &r)) return false;
    if (r.type != RedisReply::kBulk) return false;  // nil / 意外类型
    if (val) *val = std::move(r.str);
    return true;
}

bool RedisClient::del(const std::string& key) {
    RedisReply r;
    return command({"DEL", key}, &r);
}

// ---- worker 线程侧（fd_ 仅本线程访问）----

void RedisClient::workerLoop() {
    int64_t lastActiveMs = nowMs();
    while (running_.load()) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait_for(lk, std::chrono::milliseconds(500), [&] {
                return !q_.empty() || !running_.load();
            });
            if (!running_.load()) break;
            if (q_.empty()) {
                // 空闲保活：30s 一次 PING，顺带探测死连接
                if (nowMs() - lastActiveMs > 30'000 && fd_ >= 0) {
                    RedisReply r;
                    if (sendAndRecv(encodeCommand({"PING"}), &r)) {
                        recordSuccess();
                    } else {
                        closeConn();
                        recordFailure();
                    }
                    lastActiveMs = nowMs();
                }
                continue;
            }
            req = std::move(q_.front());
            q_.pop_front();
        }

        if (req.promise) {
            // 同步命令：熔断打开期间快速失败（kError → command() 判 false）
            if (breakerOpen()) {
                RedisReply r;
                r.type = RedisReply::kError;
                r.str = "circuit open";
                req.promise->set_value(std::move(r));
                continue;
            }
            if (fd_ < 0 && !connect()) {
                recordFailure();
                RedisReply r;
                r.type = RedisReply::kError;
                r.str = "connect failed";
                req.promise->set_value(std::move(r));
                continue;
            }
            RedisReply reply;
            if (!sendAndRecv(req.cmd, &reply)) {
                closeConn();
                recordFailure();
                RedisReply r;
                r.type = RedisReply::kError;
                r.str = "io error";
                req.promise->set_value(std::move(r));
                continue;
            }
            recordSuccess();
            lastActiveMs = nowMs();
            req.promise->set_value(std::move(reply));
        } else {
            // 异步命令（尽力而为）：连接失败直接丢弃
            if (breakerOpen()) continue;
            if (fd_ < 0 && !connect()) {
                recordFailure();
                continue;
            }
            RedisReply reply;
            if (!sendAndRecv(req.cmd, &reply)) {
                closeConn();
                recordFailure();
                continue;
            }
            recordSuccess();
            lastActiveMs = nowMs();
        }
    }
    closeConn();
}

bool RedisClient::connect() {
    std::string host;
    uint16_t port;
    int timeoutMs;
    std::string password;
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        host = host_;
        port = port_;
        timeoutMs = timeoutMs_;
        password = password_;
    }
    if (host.empty()) return false;

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints,
                      &res) != 0 ||
        !res) {
        return false;
    }
    int fd = -1;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) return false;
    fd_ = fd;

    if (!password.empty()) {
        RedisReply r;
        if (!sendAndRecv(encodeCommand({"AUTH", password}), &r) ||
            r.isError()) {
            closeConn();
            return false;
        }
    }
    return true;
}

void RedisClient::closeConn() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool RedisClient::sendAll(const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd_, data.data() + off, data.size() - off,
                           MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool RedisClient::sendAndRecv(const std::string& cmd, RedisReply* out) {
    if (fd_ < 0) return false;
    if (!sendAll(cmd)) return false;
    std::string buf;
    char tmp[16384];
    for (;;) {
        int used = parseReply(buf.data(), buf.size(), out);
        if (used > 0) return true;
        if (used < 0) return false;  // 协议错误
        ssize_t r = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;  // 超时(EAGAIN) / 对端关闭
        buf.append(tmp, static_cast<size_t>(r));
    }
}

void RedisClient::recordFailure() {
    int n = consecutiveFailures_.fetch_add(1) + 1;
    if (n == kBreakerThreshold) {
        breakerOpenUntilMs_.store(nowMs() + kBreakerCooldownMs);
        LOG_WARN("RedisClient: circuit OPEN for %lldms after %d failures",
                 static_cast<long long>(kBreakerCooldownMs), n);
    }
}

void RedisClient::recordSuccess() {
    if (breakerOpenUntilMs_.load() != 0) {
        LOG_INFO("RedisClient: circuit CLOSED (recovered)");
    }
    consecutiveFailures_.store(0);
    breakerOpenUntilMs_.store(0);
}

bool RedisClient::breakerOpen() const {
    return nowMs() < breakerOpenUntilMs_.load();
}

}  // namespace streaming
