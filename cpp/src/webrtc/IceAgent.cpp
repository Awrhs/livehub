#include "webrtc/IceAgent.hpp"
#include "core/Logger.hpp"

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

IceAgent::~IceAgent() { stop(); }

void IceAgent::start() { udp_->start(); }

void IceAgent::stop() {
    udp_.reset();
    selected_ = false;
}

std::string IceAgent::hostCandidate(const std::string& publicIp) const {
    std::string ip = publicIp.empty() ? "127.0.0.1" : publicIp;
    // candidate:<foundation> <component> <proto> <priority> <ip> <port> typ host
    return "candidate:1 1 udp 2130706431 " + ip + " " +
           std::to_string(udp_ ? udp_->port() : 0) + " typ host";
}

void IceAgent::send(const uint8_t* data, size_t len) {
    if (!selected_) return;
    udp_->sendto(data, len, peer_);
}

void IceAgent::onDatagram(const uint8_t* data, size_t len,
                          const sockaddr_in& peer, Timestamp) {
    // STUN 报文首 2 位为 0（RFC 5389）
    bool isStun = len >= 20 && (data[0] & 0xC0) == 0 &&
                  data[4] == 0x21 && data[5] == 0x12 && data[6] == 0xA4 &&
                  data[7] == 0x42;

    if (isStun) {
        StunMessage msg;
        if (Stun::parse(data, len, msg) &&
            msg.type == kStunBindingRequest) {
            if (!password_.empty() &&
                !Stun::checkIntegrity(data, len, password_)) {
                LOG_WARN("ICE: STUN integrity check failed");
                return;
            }
            // 锁定候选对（ICE-lite 使用首次成功 connectivity check 的地址）
            if (!selected_) {
                peer_ = peer;
                selected_ = true;
                if (onCandidate_) onCandidate_(peer);
                LOG_INFO("ICE: selected peer %s:%u",
                         inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            }
            uint32_t ipv4 = ntohl(peer.sin_addr.s_addr);
            uint16_t port = ntohs(peer.sin_port);
            auto resp = Stun::buildBindingResponse(msg, ipv4, port, password_);
            udp_->sendto(resp.data(), resp.size(), peer);
            return;
        }
    }

    // 非 STUN：必须等候选对选定后交给上层（DTLS/RTP/RTCP）
    if (selected_ && onData_) {
        onData_(data, len, peer);
    }
}

}  // namespace streaming
