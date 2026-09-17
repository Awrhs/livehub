#include "srt/SrtServer.hpp"

#include "core/Logger.hpp"
#include "net/EventLoop.hpp"

#include <arpa/inet.h>

namespace streaming {

SrtServer::SrtServer(EventLoop* loop, uint16_t port, const std::string& ip)
    : loop_(loop) {
    udp_ = std::make_unique<UdpServer>(loop, port, ip);
    udp_->setMessageCallback(
        [this](const uint8_t* d, size_t n, const sockaddr_in& peer,
               Timestamp t) { this->onMessage(d, n, peer, t); });
}

SrtServer::~SrtServer() = default;

void SrtServer::start() {
    udp_->start();
    LOG_INFO("SRT server listening on udp/%u", udp_->port());
}

std::string SrtServer::peerKey(const sockaddr_in& peer) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
}

std::shared_ptr<SrtSession> SrtServer::getOrCreate(const sockaddr_in& peer) {
    std::string key = peerKey(peer);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(key);
    if (it != sessions_.end()) return it->second;

    uint32_t sid = nextSocketId_.fetch_add(1);
    auto session = std::make_shared<SrtSession>(
        loop_, peer, sid,
        [this](const std::vector<uint8_t>& pkt, const sockaddr_in& p) {
            udp_->sendto(pkt.data(), pkt.size(), p);
        });
    if (resolver_) session->setIngestResolver(resolver_);
    session->setUnpublishCallback([this](const std::string& k) {
        if (onUnpublish_) onUnpublish_(k);
    });
    sessions_[key] = session;
    return session;
}

void SrtServer::onMessage(const uint8_t* data, size_t len,
                          const sockaddr_in& peer, Timestamp) {
    auto session = getOrCreate(peer);

    session->onPacket(data, len);
    // 握手完成后：若为拉流会话，解析 streamid 中的 r=<resource> 并绑定本地流
    if (session->connected() && session->isPlay() &&
        !session->playAttached() && playResolver_) {
        std::string sid = session->streamId();
        // streamid 形如 "#!::r=live/key,m=request"；取 r= 后的资源名
        std::string key = sid;
        auto rpos = sid.find("r=");
        if (rpos != std::string::npos) {
            size_t start = rpos + 2;
            size_t end = sid.find(',', start);
            key = sid.substr(start, end == std::string::npos ? std::string::npos
                                                             : end - start);
            auto slash = key.find('/');
            if (slash != std::string::npos) key = key.substr(slash + 1);
        }
        auto ms = playResolver_(key);
        if (ms) session->attachPlayStream(ms);
    }
}

std::shared_ptr<SrtSession> SrtServer::pushTo(
    const std::string& host, uint16_t port, const std::string& streamId,
    const std::shared_ptr<MediaStream>& src) {
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &peer.sin_addr);

    // 用一个独立的 ephemeral UDP socket
    auto client = std::make_shared<UdpServer>(loop_, 0, "0.0.0.0");
    uint32_t sid = nextSocketId_.fetch_add(1);
    // session 持有 client，保证其生命周期
    auto session = std::make_shared<SrtSession>(
        loop_, peer, sid,
        [client](const std::vector<uint8_t>& pkt, const sockaddr_in& p) {
            client->sendto(pkt.data(), pkt.size(), p);
        });
    client->setMessageCallback(
        [session](const uint8_t* d, size_t n, const sockaddr_in&, Timestamp) {
            session->onPacket(d, n);
        });
    client->start();
    session->connectCaller(streamId);
    session->attachPlayStream(src);

    std::string key = peerKey(peer);
    std::lock_guard<std::mutex> lk(mu_);
    sessions_[key + ":caller"] = session;
    return session;
}

size_t SrtServer::sessionCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}

}  // namespace streaming
