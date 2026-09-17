#pragma once

#include <cstdint>
#include <string>

namespace streaming {

/**
 * 为一路 H264/AAC 直播流生成 RTSP DESCRIBE 响应 SDP。
 *
 * 视频 payload type 96 (H264/90000)，音频 97 (MPEG4-GENERIC/44100/2)，
 * 控制 track 路径 trackID=0 / trackID=1。
 */
std::string buildRtspSdp(const std::string& streamUrl,
                         const std::string& spsBase64,
                         const std::string& ppsBase64,
                         uint32_t audioSampleRate = 44100);

}  // namespace streaming
