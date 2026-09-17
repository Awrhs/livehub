#include "webrtc/Sdp.hpp"

#include <sstream>

namespace streaming {

static std::string joinPayloads(const std::vector<int>& pts) {
    std::string s;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i) s += " ";
        s += std::to_string(pts[i]);
    }
    return s;
}

std::string SdpMessage::serialize() const {
    std::ostringstream o;
    o << "v=0\r\n";
    o << "o=" << userName << " " << sessionId << " " << sessionVersion
      << " IN IP4 " << originAddr << "\r\n";
    o << "s=streaming-cpp\r\n";
    o << "t=0 0\r\n";
    o << "c=IN IP4 " << connectionIp << "\r\n";

    for (const auto& m : medias) {
        o << "m=" << m.type << " " << m.port << " " << m.proto << " "
          << joinPayloads(m.payloadTypes) << "\r\n";
        if (!m.clineIp.empty()) o << "c=IN IP4 " << m.clineIp << "\r\n";
        if (!m.ufrag.empty()) o << "a=ice-ufrag:" << m.ufrag << "\r\n";
        if (!m.pwd.empty()) o << "a=ice-pwd:" << m.pwd << "\r\n";
        o << "a=ice-options:trickle\r\n";
        if (m.rtcpMux) o << "a=rtcp-mux\r\n";
        if (!m.direction.empty()) o << "a=" << m.direction << "\r\n";
        if (!m.dtlsSetup.empty() && !m.fingerprintHash.empty()) {
            o << "a=setup:" << m.dtlsSetup << "\r\n";
            o << "a=fingerprint:" << m.fingerprintHash << "\r\n";
        }
        for (const auto& kv : m.rtpmap) {
            o << "a=rtpmap:" << kv.first << " " << kv.second << "\r\n";
        }
        for (const auto& kv : m.fmtp) {
            o << "a=fmtp:" << kv.first << " " << kv.second << "\r\n";
        }
        if (!m.rtcpFb.empty()) {
            for (const auto& pt : m.payloadTypes) {
                o << "a=rtcp-fb:" << pt << " " << m.rtcpFb << "\r\n";
            }
        }
        if (m.ssrc != 0) {
            o << "a=ssrc:" << m.ssrc << " cname:" << m.ssrcCname << "\r\n";
        }
    }
    return o.str();
}

// 最小化解析：提取每个 m= 块的 ice-ufrag / ice-pwd / fingerprint / setup
bool SdpMessage::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    SdpMedia* cur = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2 || line[1] != '=') continue;
        char t = line[0];
        std::string val = line.substr(2);
        if (t == 'o') {
            std::istringstream ls(val);
            std::string user, net, ipver;
            ls >> user >> sessionId >> sessionVersion >> net >> ipver >>
                originAddr;
            userName = user;
        } else if (t == 'm') {
            SdpMedia m;
            std::istringstream ls(val);
            ls >> m.type >> m.port >> m.proto;
            int pt;
            while (ls >> pt) m.payloadTypes.push_back(pt);
            medias.push_back(m);
            cur = &medias.back();
        } else if (t == 'a' && cur) {
            auto pos = val.find(':');
            std::string key = pos == std::string::npos ? val : val.substr(0, pos);
            std::string v = pos == std::string::npos ? "" : val.substr(pos + 1);
            if (key == "ice-ufrag") cur->ufrag = v;
            else if (key == "ice-pwd") cur->pwd = v;
            else if (key == "fingerprint") cur->fingerprintHash = v;
            else if (key == "setup") cur->dtlsSetup = v;
            else if (key == "sendonly" || key == "recvonly" ||
                     key == "sendrecv" || key == "inactive")
                cur->direction = key;
        }
    }
    return !medias.empty();
}

}  // namespace streaming
