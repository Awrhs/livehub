#include "webrtc/WebRtcUtil.hpp"

#include "api/HttpRequest.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

namespace streaming {

namespace {
std::string detectIpv4() {
    std::string ip = "127.0.0.1";
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return ip;
    for (ifaddrs* p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        if ((a >> 24) == 127) continue;
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            ip = buf;
            break;
        }
    }
    freeifaddrs(ifs);
    return ip;
}
}  // namespace

std::string webrtcCandidateLine(const std::string& ip, uint16_t port) {
    static const std::string detected = detectIpv4();
    const std::string& addr = ip.empty() ? detected : ip;
    return "a=candidate:1 1 udp 2130706431 " + addr + " " + std::to_string(port) + " typ host";
}

std::string webrtcRequestToken(const HttpRequest& req) {
    std::string auth = req.header("Authorization");
    if (auth.compare(0, 7, "Bearer ") == 0) return auth.substr(7);
    return req.param("token");
}

}  // namespace streaming
