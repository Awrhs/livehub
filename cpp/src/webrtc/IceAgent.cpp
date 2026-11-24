#include "webrtc/IceAgent.hpp"
#include "core/Logger.hpp"
#include "webrtc/IceMux.hpp"

#include <arpa/inet.h>

#include <cstring>

namespace streaming {

IceAgent::IceAgent(EventLoop* loop, uint16_t port) : loop_(loop) {
    udp_ = std::make_unique<UdpServer>(loop, port);
    udp_->setMessageCallback(
        [this](const uint8_t* d, size_t n, const sockaddr_in& p, Timestamp t) {
            onDatagram(d, n, p, t);
        });
}

IceAgent::IceAgent(EventLoop* loop, IceMux* mux, std::string localUfrag)
    : loop_(loop), mux_(mux), ufrag_(std::move(localUfrag)) {}

IceAgent::~IceAgent() { stop(); }

void IceAgent::start() {
    lastRecvMs_ = steadyMs();
    if (mux_) mux_->add(ufrag_, this);
    else if (udp_) udp_->start();
}

void IceAgent::stop() {
    if (mux_) {
        mux_->remove(this);
        mux_ = nullptr;
    }
    udp_.reset();
    selected_ = false;
}

uint16_t IceAgent::port() const {
    if (mux_) return mux_->port();
    return udp_ ? udp_->port() : 0;
}

std::string IceAgent::hostCandidate(const std::string& publicIp) const {
    std::string ip = publicIp.empty() ? "127.0.0.1" : publicIp;
    // candidate:<foundation> <component> <proto> <priority> <ip> <port> typ host
    return "candidate:1 1 udp 2130706431 " + ip + " " + std::to_string(port()) + " typ host";
}

void IceAgent::sendRaw(const uint8_t* data, size_t len, const sockaddr_in& peer) {
    if (mux_) mux_->sendto(data, len, peer);
    else if (udp_) udp_->sendto(data, len, peer);
}

void IceAgent::send(const uint8_t* data, size_t len) {
    if (!selected_) return;
    sendRaw(data, len, peer_);
}

// 处理 STUN Binding Request：校验完整性、锁定候选对、回复 Binding Success。
// 返回 true 表示这是本会话的合法连通性检查。
bool IceAgent::acceptStun(const uint8_t* data, size_t len, const sockaddr_in& peer) {
    lastRecvMs_ = steadyMs();
    StunMessage msg;
    if (!Stun::parse(data, len, msg) || msg.type != kStunBindingRequest) return false;
    if (!password_.empty() && !Stun::checkIntegrity(data, len, password_)) {
        LOG_WARN("ICE: STUN integrity check failed");
        return false;
    }
    // 锁定候选对（ICE-lite 使用首次成功 connectivity check 的地址）
    if (!selected_) {
        peer_ = peer;
        selected_ = true;
        if (onCandidate_) onCandidate_(peer);
        LOG_INFO("ICE: selected peer %s:%u", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    }
    uint32_t ipv4 = ntohl(peer.sin_addr.s_addr);
    uint16_t port = ntohs(peer.sin_port);
    auto resp = Stun::buildBindingResponse(msg, ipv4, port, password_);
    sendRaw(resp.data(), resp.size(), peer);
    return true;
}

void IceAgent::onMuxDatagram(const uint8_t* data, size_t len, const sockaddr_in& peer, Timestamp t) {
    onDatagram(data, len, peer, t);
}

void IceAgent::onDatagram(const uint8_t* data, size_t len,
                          const sockaddr_in& peer, Timestamp) {
    lastRecvMs_ = steadyMs();
    // STUN 报文首 2 位为 0（RFC 5389）
    bool isStun = len >= 20 && (data[0] & 0xC0) == 0 &&
                  data[4] == 0x21 && data[5] == 0x12 && data[6] == 0xA4 &&
                  data[7] == 0x42;
    if (isStun) {
        acceptStun(data, len, peer);
        return;
    }
    // 非 STUN：必须等候选对选定后交给上层（DTLS/RTP/RTCP）
    if (selected_ && onData_) {
        onData_(data, len, peer);
    }
}

}  // namespace streaming
