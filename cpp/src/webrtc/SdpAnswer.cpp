#include "webrtc/SdpAnswer.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <vector>

namespace streaming {

namespace {

struct OfferSection {
    std::string media;   // video / audio / application
    std::string proto;
    std::vector<int> pts;
    std::string mid;
    std::map<int, std::string> rtpmap;  // pt -> "H264/90000"
    std::map<int, std::string> fmtp;
};

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<OfferSection> parseOffer(const std::string& offer) {
    std::vector<OfferSection> out;
    std::istringstream is(offer);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 2, "m=") == 0) {
            OfferSection s;
            std::istringstream ls(line.substr(2));
            std::string port;
            ls >> s.media >> port >> s.proto;
            int pt;
            while (ls >> pt) s.pts.push_back(pt);
            out.push_back(std::move(s));
            continue;
        }
        if (out.empty()) continue;
        OfferSection& s = out.back();
        if (line.compare(0, 6, "a=mid:") == 0) {
            s.mid = line.substr(6);
        } else if (line.compare(0, 9, "a=rtpmap:") == 0) {
            auto sp = line.find(' ');
            if (sp == std::string::npos) continue;
            s.rtpmap[std::atoi(line.c_str() + 9)] = line.substr(sp + 1);
        } else if (line.compare(0, 7, "a=fmtp:") == 0) {
            auto sp = line.find(' ');
            if (sp == std::string::npos) continue;
            s.fmtp[std::atoi(line.c_str() + 7)] = line.substr(sp + 1);
        }
    }
    return out;
}

// 选择 H.264 PT：要求 packetization-mode=1，优先 Constrained Baseline（42e0xx/42e01f）
int pickH264(const OfferSection& s) {
    int best = -1, bestScore = -1;
    for (int pt : s.pts) {
        auto it = s.rtpmap.find(pt);
        if (it == s.rtpmap.end() || lower(it->second).compare(0, 5, "h264/") != 0) continue;
        std::string f = lower(s.fmtp.count(pt) ? s.fmtp.at(pt) : "");
        if (f.find("packetization-mode=1") == std::string::npos) continue;
        int score = 1;
        if (f.find("profile-level-id=42e0") != std::string::npos) score = 3;
        else if (f.find("profile-level-id=42") != std::string::npos) score = 2;
        if (score > bestScore) {
            bestScore = score;
            best = pt;
        }
    }
    return best;
}

}  // namespace

SdpAnswerResult buildSdpAnswer(const std::string& offer, const SdpAnswerParams& p) {
    SdpAnswerResult r;
    auto sections = parseOffer(offer);
    if (sections.empty()) {
        r.error = "offer has no media";
        return r;
    }

    // 先确定每个 m 行是否接受：首个含 H.264 的视频 → 收/发；含 Opus 的音频 → inactive 接受
    //（音频暂不收发；以 inactive 接受而非端口 0 拒绝，兼容在拒绝的 m 行上仍需编解码器信息的实现）
    std::vector<int> accepted(sections.size(), -1);  // 接受则为选定 PT
    std::vector<bool> inactive(sections.size(), false);
    for (size_t i = 0; i < sections.size(); ++i) {
        if (sections[i].media == "video" && r.videoPayloadType < 0) {
            int pt = pickH264(sections[i]);
            if (pt >= 0) {
                accepted[i] = pt;
                r.videoPayloadType = pt;
            }
        } else if (sections[i].media == "audio") {
            for (int pt : sections[i].pts) {
                auto it = sections[i].rtpmap.find(pt);
                if (it != sections[i].rtpmap.end() &&
                    lower(it->second).compare(0, 5, "opus/") == 0) {
                    accepted[i] = pt;
                    inactive[i] = true;
                    break;
                }
            }
        }
    }
    if (r.videoPayloadType < 0) {
        r.error = "offer has no H.264 (packetization-mode=1) video";
        return r;
    }

    std::ostringstream o;
    o << "v=0\r\n";
    o << "o=- " << p.ssrc << " 2 IN IP4 127.0.0.1\r\n";
    o << "s=streaming-server\r\n";
    o << "t=0 0\r\n";
    o << "a=ice-lite\r\n";
    // ICE 凭据与 DTLS 指纹放在会话级（对所有 m 行生效；部分实现在被拒绝的 m 行上也要求能解析到）
    o << "a=ice-ufrag:" << p.ufrag << "\r\n";
    o << "a=ice-pwd:" << p.pwd << "\r\n";
    o << "a=fingerprint:" << p.fingerprint << "\r\n";
    o << "a=setup:passive\r\n";
    o << "a=group:BUNDLE";
    for (size_t i = 0; i < sections.size(); ++i)
        if (accepted[i] >= 0) o << " " << sections[i].mid;
    o << "\r\n";
    o << "a=msid-semantic: WMS\r\n";

    bool first = true;
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        if (accepted[i] < 0) {
            // 拒绝：端口 0，保留一个 PT 与 mid
            o << "m=" << s.media << " 0 " << (s.proto.empty() ? "UDP/TLS/RTP/SAVPF" : s.proto)
              << " " << (s.pts.empty() ? 0 : s.pts[0]) << "\r\n";
            o << "c=IN IP4 0.0.0.0\r\n";
            if (!s.mid.empty()) o << "a=mid:" << s.mid << "\r\n";
            o << "a=inactive\r\n";
            if (!s.pts.empty() && s.rtpmap.count(s.pts[0]))
                o << "a=rtpmap:" << s.pts[0] << " " << s.rtpmap.at(s.pts[0]) << "\r\n";
            o << "a=rtcp-mux\r\n";  // 部分实现（aiortc）对所有 m 行都要求 rtcp-mux
            continue;
        }
        int pt = accepted[i];
        o << "m=" << s.media << " " << p.port << " UDP/TLS/RTP/SAVPF " << pt << "\r\n";
        o << "c=IN IP4 0.0.0.0\r\n";
        o << "a=rtcp:9 IN IP4 0.0.0.0\r\n";
        o << "a=ice-ufrag:" << p.ufrag << "\r\n";
        o << "a=ice-pwd:" << p.pwd << "\r\n";
        o << "a=fingerprint:" << p.fingerprint << "\r\n";
        o << "a=setup:passive\r\n";
        o << "a=mid:" << s.mid << "\r\n";
        if (inactive[i]) o << "a=inactive\r\n";
        else o << (p.sendVideo ? "a=sendonly\r\n" : "a=recvonly\r\n");
        o << "a=rtcp-mux\r\n";
        o << "a=rtpmap:" << pt << " " << s.rtpmap.at(pt) << "\r\n";
        if (s.fmtp.count(pt)) o << "a=fmtp:" << pt << " " << s.fmtp.at(pt) << "\r\n";
        if (!inactive[i]) {
            o << "a=rtcp-fb:" << pt << " nack\r\n";
            o << "a=rtcp-fb:" << pt << " nack pli\r\n";
        }
        if (p.sendVideo && p.ssrc && !inactive[i]) {
            o << "a=msid:streaming video0\r\n";
            o << "a=ssrc:" << p.ssrc << " cname:streaming\r\n";
            o << "a=ssrc:" << p.ssrc << " msid:streaming video0\r\n";
        }
        if (first && !p.candidateLine.empty()) {
            o << p.candidateLine << "\r\n";
            o << "a=end-of-candidates\r\n";
        }
        first = false;
    }
    r.sdp = o.str();
    r.ok = true;
    return r;
}

}  // namespace streaming
