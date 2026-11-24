#include "webrtc/IceMux.hpp"

#include "core/Logger.hpp"
#include "webrtc/IceAgent.hpp"
#include "webrtc/Stun.hpp"

namespace streaming {

IceMux::IceMux(EventLoop* loop, uint16_t port) : port_(port) {
    udp_ = std::make_unique<UdpServer>(loop, port);
    udp_->setMessageCallback(
        [this](const uint8_t* d, size_t n, const sockaddr_in& p, Timestamp t) { onDatagram(d, n, p, t); });
}

IceMux::~IceMux() = default;

void IceMux::start() {
    udp_->start();
    LOG_INFO("IceMux: WebRTC 单端口复用 UDP %u", port_);
}

void IceMux::add(const std::string& localUfrag, IceAgent* agent) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    byUfrag_[localUfrag] = agent;
}

void IceMux::remove(IceAgent* agent) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    for (auto it = byUfrag_.begin(); it != byUfrag_.end();) {
        if (it->second == agent) it = byUfrag_.erase(it);
        else ++it;
    }
    for (auto it = byAddr_.begin(); it != byAddr_.end();) {
        if (it->second == agent) it = byAddr_.erase(it);
        else ++it;
    }
}

size_t IceMux::sessionCount() const {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return byUfrag_.size();
}

void IceMux::sendto(const uint8_t* data, size_t len, const sockaddr_in& peer) {
    udp_->sendto(data, len, peer);
}

void IceMux::onDatagram(const uint8_t* data, size_t len, const sockaddr_in& peer, Timestamp t) {
    bool isStun = len >= 20 && (data[0] & 0xC0) == 0 && data[4] == 0x21 && data[5] == 0x12 &&
                  data[6] == 0xA4 && data[7] == 0x42;
    std::lock_guard<std::recursive_mutex> lk(mu_);
    IceAgent* agent = nullptr;
    if (isStun) {
        StunMessage msg;
        if (Stun::parse(data, len, msg)) {
            std::string user = msg.username();
            auto colon = user.find(':');
            auto it = byUfrag_.find(user.substr(0, colon));
            if (it != byUfrag_.end()) {
                agent = it->second;
                // 校验通过与否由会话自己判断；记录地址映射供后续 DTLS/RTP 分发
                if (agent->acceptStun(data, len, peer)) byAddr_[addrKey(peer)] = agent;
                return;
            }
        }
    }
    auto it = byAddr_.find(addrKey(peer));
    if (it != byAddr_.end()) agent = it->second;
    if (agent) agent->onMuxDatagram(data, len, peer, t);
}

}  // namespace streaming
