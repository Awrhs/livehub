#include "hls/TsPacket.hpp"

#include <cstring>

namespace streaming {

// ---- CRC32/MPEG-2 ----
uint32_t TsPacket::crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80000000) {
                crc = (crc << 1) ^ 0x04C11DB7;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void TsPacket::writeHeader(uint8_t* out, uint16_t pid, uint8_t cc, bool pusi,
                           bool hasAdaptation, bool hasPayload) {
    out[0] = kTsSync;
    // byte1: TEI(1) PUSI(1) priority(1) PID[12:8](5)
    // 原实现在此多或了 0x10（即 PID 第 12 位），导致所有 PID +0x1000、PAT 不在 PID 0
    out[1] = static_cast<uint8_t>((pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    out[2] = static_cast<uint8_t>(pid & 0xFF);
    uint8_t afc = 0;
    if (hasAdaptation) afc |= 0x20;
    if (hasPayload) afc |= 0x10;
    out[3] = afc | (cc & 0x0F);
}

// PCR：33 位 base（90kHz）+ 6 位保留(全 1) + 9 位 ext（27MHz 余数）
static void writePcr(uint8_t* p, int64_t pcr90k) {
    uint64_t base = static_cast<uint64_t>(pcr90k) & 0x1FFFFFFFFULL;
    uint16_t ext = 0;
    p[0] = static_cast<uint8_t>(base >> 25);
    p[1] = static_cast<uint8_t>(base >> 17);
    p[2] = static_cast<uint8_t>(base >> 9);
    p[3] = static_cast<uint8_t>(base >> 1);
    p[4] = static_cast<uint8_t>(((base & 1) << 7) | 0x7E | ((ext >> 8) & 1));
    p[5] = static_cast<uint8_t>(ext & 0xFF);
}

size_t TsPacket::buildPat(uint8_t* out, uint16_t pmtPid, uint8_t cc) {
    uint8_t section[188];
    section[0] = 0x00;  // pointer field
    section[1] = 0x00;  // table_id PAT
    // section_length: 5(tsid..last) + 4(program loop) + 4(crc) = 13
    section[2] = 0xB0;
    section[3] = 0x0D;
    section[4] = 0x00;
    section[5] = 0x01;  // transport_stream_id
    section[6] = 0xC1;  // version 0, current_next=1
    section[7] = 0x00;  // section_number
    section[8] = 0x00;  // last_section_number
    // program 1 -> PMT PID
    section[9] = 0x00;
    section[10] = 0x01;
    section[11] = static_cast<uint8_t>(0xE0 | ((pmtPid >> 8) & 0x1F));
    section[12] = static_cast<uint8_t>(pmtPid & 0xFF);

    uint32_t crc = crc32(&section[1], 12);  // table_id..last program byte
    section[13] = static_cast<uint8_t>(crc >> 24);
    section[14] = static_cast<uint8_t>(crc >> 16);
    section[15] = static_cast<uint8_t>(crc >> 8);
    section[16] = static_cast<uint8_t>(crc);
    size_t sectionLen = 17;

    // TS 头 + payload
    writeHeader(out, kPatPid, cc, true, false, true);
    size_t pos = 4;
    std::memcpy(out + pos, section, sectionLen);
    pos += sectionLen;
    while (pos < kTsPacketSize) out[pos++] = 0xFF;
    return kTsPacketSize;
}

size_t TsPacket::buildPmt(uint8_t* out, uint16_t pmtPid, uint16_t pcrPid,
                          uint16_t videoPid, uint16_t audioPid, uint8_t cc,
                          uint8_t videoStreamType) {
    uint8_t section[188];
    section[0] = 0x00;  // pointer field
    section[1] = 0x02;  // table_id PMT

    // 先写可变的 ES 列表，再回填 section_length
    size_t p = 13;
    auto writeEs = [&](uint8_t streamType, uint16_t pid) {
        section[p++] = streamType;
        section[p++] = static_cast<uint8_t>(0xE0 | ((pid >> 8) & 0x1F));
        section[p++] = static_cast<uint8_t>(pid & 0xFF);
        section[p++] = 0xF0;
        section[p++] = 0x00;  // ES info length 0
    };
    writeEs(videoStreamType, videoPid);
    if (audioPid != 0) writeEs(kStreamTypeAudio, audioPid);
    // section_length 字段不含前 2 字节，含 version/section 等：
    // 5 (program..last_section) + 4 (pcr + program_info_length) + N*5(ES) + 4 crc
    size_t bodyLen = 5 + 4 + (p - 13) + 4;

    section[2] = static_cast<uint8_t>(0xB0 | ((bodyLen >> 8) & 0x0F));
    section[3] = static_cast<uint8_t>(bodyLen & 0xFF);
    section[4] = 0x00;
    section[5] = 0x01;  // program_number
    section[6] = 0xC1;  // version/cni
    section[7] = 0x00;
    section[8] = 0x00;
    section[9] = static_cast<uint8_t>(0xE0 | ((pcrPid >> 8) & 0x1F));
    section[10] = static_cast<uint8_t>(pcrPid & 0xFF);
    section[11] = 0xF0;  // program_info_length = 0
    section[12] = 0x00;

    uint32_t crc = crc32(&section[1], p - 1);
    section[p++] = static_cast<uint8_t>(crc >> 24);
    section[p++] = static_cast<uint8_t>(crc >> 16);
    section[p++] = static_cast<uint8_t>(crc >> 8);
    section[p++] = static_cast<uint8_t>(crc);

    writeHeader(out, pmtPid, cc, true, false, true);
    size_t pos = 4;
    std::memcpy(out + pos, section, p);
    pos += p;
    while (pos < kTsPacketSize) out[pos++] = 0xFF;
    return kTsPacketSize;
}

size_t TsPacket::buildPes(uint8_t* out, uint16_t pid, uint8_t cc, bool pusi,
                          const uint8_t* payload, size_t payloadLen,
                          int64_t pcr, bool randomAccess, size_t* consumed) {
    // adaptation field 总长度（含 length 字节）
    bool hasPcr = pcr >= 0;
    size_t afLen = (hasPcr || randomAccess) ? (2 + (hasPcr ? 6 : 0)) : 0;
    size_t space = kTsPacketSize - 4 - afLen;
    size_t n = payloadLen < space ? payloadLen : space;
    // 负载不足一个包时，必须用 adaptation field 填充（stuffing），
    // 绝不能在 payload 末尾补 0xFF —— 那会被解码器当作 ES 数据（原实现的 bug）。
    if (n < space) afLen += space - n;

    writeHeader(out, pid, cc, pusi, afLen > 0, true);
    size_t pos = 4;
    if (afLen > 0) {
        out[pos] = static_cast<uint8_t>(afLen - 1);  // adaptation_field_length
        if (afLen >= 2) {
            uint8_t flags = 0;
            if (randomAccess) flags |= 0x40;
            if (hasPcr) flags |= 0x10;
            out[pos + 1] = flags;
            size_t q = pos + 2;
            if (hasPcr) {
                writePcr(out + q, pcr);
                q += 6;
            }
            while (q < pos + afLen) out[q++] = 0xFF;  // stuffing
        }
        pos += afLen;
    }
    if (payload && n > 0) {
        std::memcpy(out + pos, payload, n);
        pos += n;
    }
    if (consumed) *consumed = n;
    return kTsPacketSize;
}

}  // namespace streaming
