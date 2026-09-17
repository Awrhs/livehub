#include "rtsp/RtspSdp.hpp"

#include <sstream>

namespace streaming {

std::string buildRtspSdp(const std::string& streamUrl,
                         const std::string& spsBase64,
                         const std::string& ppsBase64,
                         uint32_t audioSampleRate) {
    std::ostringstream sdp;
    sdp << "v=0\r\n";
    sdp << "o=- 0 0 IN IP4 0.0.0.0\r\n";
    sdp << "s=streaming-cpp rtsp\r\n";
    sdp << "t=0 0\r\n";
    sdp << "a=control:*\r\n";
    sdp << "a=range:npt=now-\r\n";

    // video (H264)
    sdp << "m=video 0 RTP/AVP 96\r\n";
    sdp << "c=IN IP4 0.0.0.0\r\n";
    sdp << "b=AS:2000\r\n";
    sdp << "a=rtpmap:96 H264/90000\r\n";
    if (!spsBase64.empty() && !ppsBase64.empty()) {
        sdp << "a=fmtp:96 packetization-mode=1;"
               "sprop-parameter-sets="
            << spsBase64 << "," << ppsBase64 << "\r\n";
    } else {
        sdp << "a=fmtp:96 packetization-mode=1\r\n";
    }
    sdp << "a=control:trackID=0\r\n";

    // audio (AAC)
    sdp << "m=audio 0 RTP/AVP 97\r\n";
    sdp << "c=IN IP4 0.0.0.0\r\n";
    sdp << "a=rtpmap:97 MPEG4-GENERIC/" << audioSampleRate << "/2\r\n";
    sdp << "a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;"
           "sizelength=13;indexlength=3;indexdeltalength=3;config=1210\r\n";
    sdp << "a=control:trackID=1\r\n";

    (void)streamUrl;
    return sdp.str();
}

}  // namespace streaming
