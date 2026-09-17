#include "cluster/HealthChecker.hpp"

#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "net/Channel.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace streaming {

namespace {
int64_t nowMs() {
    return Timestamp::now().microSecondsSinceEpoch() / 1000;
}
}  // namespace

// 单次探测：非阻塞 connect + 发送 HTTP/1.0 GET，读首行判断 2xx，
// 超时由 HealthChecker 的 runAfter 计时强制关闭。
struct Probe {
    HealthChecker* owner;
    std::string nodeId;
    EventLoop* loop;
    int fd = -1;
    std::shared_ptr<Channel> channel;
    uint64_t timerId = 0;
    std::string in;
    bool done = false;

    Probe(HealthChecker* o, std::string id, EventLoop* l)
        : owner(o), nodeId(std::move(id)), loop(l) {}

    void finish(bool ok) {
        if (done) return;
        done = true;
        if (timerId) {
            // timer 取消由 owner 完成（保留接口）
        }
        if (channel) {
            channel->disableAll();
            channel->remove();
            channel.reset();
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        owner->onProbeResult(nodeId, ok);
        delete this;
    }
};

HealthChecker::HealthChecker(EventLoop* loop, NodeRegistry* registry,
                              int intervalSec, int failureThreshold,
                              int cooldownSec, int timeoutMs)
    : loop_(loop),
      registry_(registry),
      intervalSec_(intervalSec),
      failureThreshold_(failureThreshold),
      cooldownSec_(cooldownSec),
      timeoutMs_(timeoutMs) {}

HealthChecker::~HealthChecker() {
    stop();
}

void HealthChecker::start() {
    if (running_) return;
    running_ = true;
    // 立即探测一次，然后周期执行
    loop_->runInLoop([this] { checkAll(); });
    timerId_ = 0;
    // runEvery 返回 TimerId；此处只持有 0 简化（stop 时停止 loop 即可）
    (void)loop_->runEvery(static_cast<double>(intervalSec_),
                          [this] { checkAll(); });
}

void HealthChecker::stop() {
    running_ = false;
}

void HealthChecker::checkAll() {
    if (!running_) return;
    for (const auto& node : registry_->all()) {
        if (!node) continue;
        node->lastCheckMs.store(nowMs());
        // 熔断 OPEN 状态在冷却期内不主动探测
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = circuits_.find(node->id);
            if (it != circuits_.end() &&
                it->second.state == CircuitState::kOpen &&
                nowMs() - it->second.openedAtMs < cooldownSec_ * 1000LL) {
                continue;
            }
        }
        probeNode(node);
    }
}

void HealthChecker::probeNode(const NodePtr& node) {
    auto* p = new Probe(this, node->id, loop_);
    p->fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (p->fd < 0) {
        p->finish(false);
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(node->httpPort);
    if (::inet_pton(AF_INET, node->ip.c_str(), &addr.sin_addr) != 1) {
        p->finish(false);
        return;
    }
    int ret = ::connect(p->fd, reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        p->finish(false);
        return;
    }

    p->channel = std::make_shared<Channel>(loop_, p->fd);
    std::string host = node->ip + ":" + std::to_string(node->httpPort);
    std::string req = "GET " + healthPath_ +
                      " HTTP/1.0\r\nHost: " + host +
                      "\r\nConnection: close\r\n\r\n";

    p->channel->setWriteCallback([p, req]() {
        if (p->done) return;
        ::send(p->fd, req.data(), req.size(), MSG_NOSIGNAL);
        p->channel->disableWriting();
        p->channel->enableReading();
    });
    p->channel->setReadCallback([p](Timestamp) {
        if (p->done) return;
        char buf[512];
        ssize_t n = ::recv(p->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            p->in.append(buf, static_cast<size_t>(n));
            // 只需状态行：HTTP/1.x 200
            if (p->in.size() >= 12) {
                bool ok = p->in.compare(0, 7, "HTTP/1.") == 0 &&
                          p->in[9] == '2';
                p->finish(ok);
            }
        } else if (n == 0) {
            bool ok = p->in.size() >= 12 &&
                      p->in.compare(0, 7, "HTTP/1.") == 0 && p->in[9] == '2';
            p->finish(ok);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            p->finish(false);
        }
    });
    p->channel->setErrorCallback([p]() { p->finish(false); });
    p->channel->setCloseCallback([p]() { p->finish(false); });
    p->channel->enableWriting();

    // 超时定时器
    p->timerId = 0;
    loop_->runAfter(timeoutMs_ / 1000.0, [p] { p->finish(false); });
}

void HealthChecker::onProbeResult(const std::string& nodeId, bool ok) {
    if (ok) {
        recordSuccess(nodeId);
    } else {
        recordFailure(nodeId);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        inFlight_.erase(nodeId);
    }
}

void HealthChecker::recordSuccess(const std::string& nodeId) {
    auto node = registry_->get(nodeId);
    CircuitState old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& s = circuits_[nodeId];
        old = s.state;
        s.successes++;
        // 任何成功都重置连续失败计数；若处于半开/开闸则恢复闭环
        s.failures = 0;
        if (s.state == CircuitState::kHalfOpen ||
            s.state == CircuitState::kOpen) {
            s.state = CircuitState::kClosed;
        }
    }
    if (node) {
        node->healthy.store(true);
        node->consecutiveFailures.store(0);
    }
    if (old != CircuitState::kClosed) transition(nodeId, CircuitState::kClosed);
}

void HealthChecker::recordFailure(const std::string& nodeId) {
    auto node = registry_->get(nodeId);
    CircuitState next = CircuitState::kClosed;
    CircuitState old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& s = circuits_[nodeId];
        old = s.state;
        s.failures++;
        if (s.state == CircuitState::kHalfOpen) {
            s.state = CircuitState::kOpen;
            s.openedAtMs = nowMs();
        } else if (s.state == CircuitState::kClosed &&
                   s.failures >= failureThreshold_) {
            s.state = CircuitState::kOpen;
            s.openedAtMs = nowMs();
        } else if (s.state == CircuitState::kOpen &&
                   nowMs() - s.openedAtMs >= cooldownSec_ * 1000LL) {
            s.state = CircuitState::kHalfOpen;
        }
        next = s.state;
    }
    if (node) {
        node->consecutiveFailures.fetch_add(1);
        if (next == CircuitState::kOpen) node->healthy.store(false);
    }
    if (next != old) transition(nodeId, next);
}

void HealthChecker::transition(const std::string& nodeId,
                                CircuitState next) {
    if (onStateChange_) {
        auto node = registry_->get(nodeId);
        CircuitState old = CircuitState::kClosed;
        {
            std::lock_guard<std::mutex> lk(mu_);
            old = circuits_[nodeId].state;  // note: already updated
        }
        if (node) onStateChange_(node, old, next);
    }
    LOG_INFO("HealthChecker: node %s -> %s", nodeId.c_str(),
             next == CircuitState::kClosed
                 ? "CLOSED"
                 : next == CircuitState::kOpen ? "OPEN" : "HALF_OPEN");
}

CircuitStatus HealthChecker::status(const std::string& nodeId) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = circuits_.find(nodeId);
    if (it != circuits_.end()) return it->second;
    return CircuitStatus{};
}

bool HealthChecker::allowRequest(const std::string& nodeId) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = circuits_.find(nodeId);
    if (it == circuits_.end()) return true;
    return it->second.state != CircuitState::kOpen;
}

}  // namespace streaming
