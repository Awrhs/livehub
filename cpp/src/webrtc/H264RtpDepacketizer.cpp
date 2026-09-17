#include "webrtc/H264RtpDepacketizer.hpp"

#include <cstring>

namespace streaming {

void H264RtpDepacketizer::emitSingle(const uint8_t* nalu, size_t len,
                                     uint32_t ts, bool marker) {
    if (cb_) cb_(nalu, len, ts, marker);
}

void H264RtpDepacketizer::feed(const uint8_t* data, size_t len) {
    if (len < 12) return;
    uint8_t cc = data[0] & 0x0F;
    size_t headerLen = 12 + cc * 4;
    if (len < headerLen) return;
    uint8_t pt = data[1] & 0x7F;
    (void)pt;
    uint16_t seq = (uint16_t(data[2]) << 8) | data[3];
    uint32_t ts = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) |
                  (uint32_t(data[6]) << 8) | uint32_t(data[7]);
    bool marker = (data[1] & 0x80) != 0;

    const uint8_t* payload = data + headerLen;
    size_t plen = len - headerLen;
    if (plen == 0) return;

    uint8_t nalType = payload[0] & 0x1F;
    uint8_t nalRefIdc = (payload[0] >> 5) & 0x03;

    if (nalType >= 1 && nalType <= 23) {
        // Single NAL unit
        emitSingle(payload, plen, ts, marker);
    } else if (nalType == 24) {
        handleStapA(payload, plen, ts, marker);
    } else if (nalType == 28) {
        handleFuA(payload, plen, nalRefIdc, ts, marker);
    }
    // 29=FU-B (rare) 暂不处理
    (void)seq;
}

void H264RtpDepacketizer::handleStapA(const uint8_t* data, size_t len,
                                      uint32_t ts, bool marker) {
    size_t off = 1;  // skip STAP-A NAL header
    while (off + 2 <= len) {
        uint16_t naluSize = (uint16_t(data[off]) << 8) | data[off + 1];
        off += 2;
        if (off + naluSize > len) break;
        emitSingle(data + off, naluSize, ts, false);
        off += naluSize;
    }
    (void)marker;
}

void H264RtpDepacketizer::handleFuA(const uint8_t* data, size_t len,
                                    uint8_t nalRefIdc, uint32_t ts,
                                    bool marker) {
    if (len < 3) return;
    uint8_t fuHeader = data[1];
    bool start = (fuHeader & 0x80) != 0;
    bool end = (fuHeader & 0x40) != 0;
    uint8_t nalType = fuHeader & 0x1F;

    if (start) {
        fuInProgress_ = true;
        fuTs_ = ts;
        fuNalType_ = nalType;
        fuNalRefIdc_ = nalRefIdc;
        fuBuffer_.clear();
        // 重组的 NALU header
        uint8_t naluHeader = (nalRefIdc << 5) | nalType;
        fuBuffer_.push_back(naluHeader);
        fuBuffer_.insert(fuBuffer_.end(), data + 2, data + len);
    } else if (fuInProgress_ && nalType == fuNalType_ && ts == fuTs_) {
        fuBuffer_.insert(fuBuffer_.end(), data + 2, data + len);
    } else {
        // 丢片，放弃当前 FU-A
        fuInProgress_ = false;
        return;
    }

    if (end && fuInProgress_) {
        if (cb_) cb_(fuBuffer_.data(), fuBuffer_.size(), ts, marker);
        fuInProgress_ = false;
    }
}

}  // namespace streaming
