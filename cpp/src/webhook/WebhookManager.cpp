#include "webhook/WebhookManager.hpp"
#include "api/Json.hpp"
#include "core/Logger.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace streaming {

namespace {

struct ParsedUrl {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
    bool tls = false;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
    std::string rest = url;
    if (rest.rfind("https://", 0) == 0) {
        out.tls = true;
        rest = rest.substr(8);
        out.port = 443;
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    }
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    auto colon = hostport.find(':');
    if (colon == std::string::npos) {
        out.host = hostport;
    } else {
        out.host = hostport.substr(0, colon);
        out.port = static_cast<uint16_t>(
            std::stoi(hostport.substr(colon + 1)));
    }
    return !out.host.empty();
}

bool setTimeout(int fd, int timeoutMs) {
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return false;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        return false;
    return true;
}

bool sendAll(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

WebhookManager::WebhookManager(Config cfg) : cfg_(std::move(cfg)) {
    enabled_.store(cfg_.enabled);
}

WebhookManager::~WebhookManager() { stop(); }

void WebhookManager::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    worker_ = std::thread([this] { workerLoop(); });
    LOG_INFO("WebhookManager started");
}

void WebhookManager::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    qCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    LOG_INFO("WebhookManager stopped");
}

void WebhookManager::setUrl(const std::string& event,
                            const std::string& url) {
    std::lock_guard<std::mutex> lk(cfgMu_);
    if (event == "on_connect") cfg_.onConnect = url;
    else if (event == "on_close") cfg_.onClose = url;
    else if (event == "on_publish") cfg_.onPublish = url;
    else if (event == "on_unpublish") cfg_.onUnpublish = url;
    else if (event == "on_play") cfg_.onPlay = url;
    else if (event == "on_stop") cfg_.onStop = url;
    else if (event == "on_dvr") cfg_.onDvr = url;
    else if (event == "on_hls") cfg_.onHls = url;
}

void WebhookManager::setConfig(const Config& cfg) {
    std::lock_guard<std::mutex> lk(cfgMu_);
    cfg_ = cfg;
    enabled_.store(cfg_.enabled);
}

std::string WebhookManager::urlFor(const std::string& event) const {
    std::lock_guard<std::mutex> lk(cfgMu_);
    if (event == "on_connect") return cfg_.onConnect;
    if (event == "on_close") return cfg_.onClose;
    if (event == "on_publish") return cfg_.onPublish;
    if (event == "on_unpublish") return cfg_.onUnpublish;
    if (event == "on_play") return cfg_.onPlay;
    if (event == "on_stop") return cfg_.onStop;
    if (event == "on_dvr") return cfg_.onDvr;
    if (event == "on_hls") return cfg_.onHls;
    return "";
}

std::string WebhookManager::buildBody(const EventMap& fields) const {
    JsonValue root = JsonValue::object();
    for (const auto& kv : fields) {
        // 数值字段尝试以 number 写入，其余以字符串
        char* end = nullptr;
        double d = std::strtod(kv.second.c_str(), &end);
        bool numeric = end != kv.second.c_str() && *end == '\0' &&
                       !kv.second.empty();
        if (numeric) root.set(kv.first, JsonValue::number(d));
        else root.set(kv.first, JsonValue::str(kv.second));
    }
    return root.dump();
}

std::string WebhookManager::urlForEvent(const Config& cfg,
                                        const std::string& event) {
    if (event == "on_connect") return cfg.onConnect;
    if (event == "on_close") return cfg.onClose;
    if (event == "on_publish") return cfg.onPublish;
    if (event == "on_unpublish") return cfg.onUnpublish;
    if (event == "on_play") return cfg.onPlay;
    if (event == "on_stop") return cfg.onStop;
    if (event == "on_dvr") return cfg.onDvr;
    if (event == "on_hls") return cfg.onHls;
    return "";
}

void WebhookManager::notifyUrl(const std::string& url, int maxRetries,
                               const EventMap& fields) {
    if (url.empty()) return;
    auto job = std::make_shared<Job>();
    job->url = url;
    job->body = buildBody(fields);
    job->needResult = false;
    job->retries = maxRetries;
    {
        std::lock_guard<std::mutex> lk(qMu_);
        queue_.push_back(job);
    }
    qCv_.notify_one();
}

void WebhookManager::notify(const std::string& event, const EventMap& fields) {
    if (!enabled_.load()) return;
    Config cfg;
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        cfg = cfg_;
    }
    notifyUrl(urlForEvent(cfg, event), cfg.maxRetries, fields);
}

void WebhookManager::notify(const std::string& event, const EventMap& fields,
                            const Config& vhostCfg) {
    if (!enabled_.load() || !vhostCfg.enabled) return;
    notifyUrl(urlForEvent(vhostCfg, event), vhostCfg.maxRetries, fields);
}

WebhookManager::AuthResult WebhookManager::authorizeUrl(
    const std::string& url, int timeoutMs, const EventMap& fields) {
    AuthResult result;
    result.allowed = true;  // fail-open
    if (url.empty()) return result;

    auto job = std::make_shared<Job>();
    job->url = url;
    job->body = buildBody(fields);
    job->needResult = true;
    job->retries = 0;
    std::mutex m;
    std::condition_variable cv;
    job->mu = &m;
    job->cv = &cv;
    {
        std::lock_guard<std::mutex> lk(qMu_);
        queue_.push_back(job);
    }
    qCv_.notify_one();

    std::unique_lock<std::mutex> lk(m);
    if (!cv.wait_for(lk, std::chrono::milliseconds(timeoutMs + 500),
                     [&] { return job->done; })) {
        LOG_WARN("webhook authorize timeout url=%s", url.c_str());
        return result;  // fail-open
    }
    return job->result;
}

WebhookManager::AuthResult WebhookManager::authorize(
    const std::string& event, const EventMap& fields) {
    AuthResult result;
    result.allowed = true;
    if (!enabled_.load()) return result;
    Config cfg;
    {
        std::lock_guard<std::mutex> lk(cfgMu_);
        cfg = cfg_;
    }
    return authorizeUrl(urlForEvent(cfg, event), cfg.timeoutMs, fields);
}

WebhookManager::AuthResult WebhookManager::authorize(
    const std::string& event, const EventMap& fields,
    const Config& vhostCfg) {
    AuthResult result;
    result.allowed = true;
    if (!enabled_.load() || !vhostCfg.enabled) return result;
    return authorizeUrl(urlForEvent(vhostCfg, event), vhostCfg.timeoutMs,
                        fields);
}

bool WebhookManager::postJson(const std::string& url,
                              const std::string& body,
                              std::string& responseBody) {
    ParsedUrl pu;
    if (!parseUrl(url, pu)) return false;
    if (pu.tls) {
        // 无 OpenSSL 时不支持 https，记录并失败
        LOG_WARN("webhook https not supported in no-TLS build: %s",
                 url.c_str());
        return false;
    }

    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(pu.host.c_str(), std::to_string(pu.port).c_str(), &hints,
                      &res) != 0)
        return false;

    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        setTimeout(fd, cfg_.timeoutMs);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) return false;

    std::string req = "POST " + pu.path + " HTTP/1.1\r\n";
    req += "Host: " + pu.host + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body;
    bool ok = sendAll(fd, req.data(), req.size());
    if (ok) {
        char tmp[2048];
        ssize_t n;
        while ((n = ::recv(fd, tmp, sizeof(tmp), 0)) > 0)
            responseBody.append(tmp, static_cast<size_t>(n));
    }
    ::close(fd);
    return ok && !responseBody.empty();
}

bool WebhookManager::parseAllow(const std::string& responseBody,
                                AuthResult& out) {
    auto sep = responseBody.find("\r\n\r\n");
    if (sep == std::string::npos) return false;
    std::string body = responseBody.substr(sep + 4);
    JsonValue root;
    if (!JsonValue::parse(body, root)) return false;
    // 兼容 SRS 约定：code==0 放行；否则拒绝
    if (const JsonValue* code = root.find("code")) {
        if (code->isNumber()) {
            out.allowed = code->asNumber() == 0.0;
        }
    }
    if (const JsonValue* msg = root.find("msg")) {
        if (msg->isString()) out.message = msg->asString();
    } else if (const JsonValue* msg = root.find("message")) {
        if (msg->isString()) out.message = msg->asString();
    }
    if (const JsonValue* url = root.find("url")) {
        if (url->isString()) out.redirectUrl = url->asString();
    }
    return true;
}

void WebhookManager::workerLoop() {
    while (running_.load()) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait_for(lk, std::chrono::milliseconds(200),
                          [&] { return !queue_.empty() || !running_.load(); });
            if (!running_.load()) break;
            if (queue_.empty()) continue;
            job = queue_.front();
            queue_.pop_front();
        }

        std::string resp;
        bool ok = postJson(job->url, job->body, resp);
        if (ok) {
            sent_.fetch_add(1);
        } else {
            failed_.fetch_add(1);
            LOG_WARN("webhook post failed url=%s retry=%d", job->url.c_str(),
                     job->retries);
            if (job->retries > 0) {
                --job->retries;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                std::lock_guard<std::mutex> lk(qMu_);
                queue_.push_back(job);
                continue;
            }
        }

        if (job->needResult) {
            AuthResult r;
            if (ok) {
                if (!parseAllow(resp, r)) r.allowed = true;  // 解析失败 fail-open
            } else {
                r.allowed = true;  // 网络失败 fail-open
            }
            {
                std::lock_guard<std::mutex> lk(*job->mu);
                job->result = r;
                job->done = true;
            }
            job->cv->notify_one();
        }
    }
}

}  // namespace streaming
