#include "dvr/Mp4Faststart.hpp"

#include "core/Logger.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace streaming {

namespace {

// 读取 32-bit 大端
uint32_t rd32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
void wr32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}
uint64_t rd64(const uint8_t* p) {
    return (uint64_t(rd32(p)) << 32) | rd32(p + 4);
}
void wr64(uint8_t* p, uint64_t v) {
    wr32(p, uint32_t(v >> 32));
    wr32(p + 4, uint32_t(v));
}

bool fourcc(const uint8_t* p, const char* tag) {
    return std::memcmp(p, tag, 4) == 0;
}

struct Box {
    uint64_t offset;  // box 在文件中的起始（size 字段处）
    uint64_t size;    // box 总长
    uint64_t header;  // header 长度（8 或 16，largesize）
};

// 遍历顶层 box，返回 moov 在 moovBox，并计算 ftyp 之后的插入点。
// delta 为 mdat 等 box 的偏移补偿（moov 从尾部移到前部，大小变化导致的位移）。
//
// 实际策略：重排为 [所有非 moov 且非 ftyp 的 box][moov] 会改变 mdat 位置；
// 我们采用 [ftyp][moov][其余原序]，需要把原来位于 moov 之前的 box（除 ftyp）
// 视为向后移动 moovSize，把原来位于 moov 之后的 box 视为位置不变 —— 但 mdat
// 通常在 moov 之前（录制时先写 mdat），所以 mdat 会被后移 moovSize。
// stco/co64 的 chunk offset 加上该 delta 即可。

constexpr const char* kMoov = "moov";
constexpr const char* kStco = "stco";
constexpr const char* kCo64 = "co64";

// 递归在 moov 中找 stco/co64 并修正 chunk offset
void patchOffsets(uint8_t* p, size_t len, int64_t delta) {
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t size = rd32(p + i);
        size_t hdr = 8;
        if (size == 1) {
            if (i + 16 > len) break;
            size = rd64(p + i + 8);
            hdr = 16;
        } else if (size == 0) {
            size = len - i;  // 延伸到文件尾
        }
        if (size < hdr || i + size > len) break;
        const uint8_t* type = p + i + 4;
        uint8_t* payload = p + i + hdr;
        size_t payloadLen = static_cast<size_t>(size - hdr);

        if (fourcc(type, kStco) && payloadLen >= 8) {
            uint32_t entryCount = rd32(payload + 4);
            uint8_t* entries = payload + 8;
            for (uint32_t e = 0; e < entryCount; ++e) {
                uint32_t off = rd32(entries + e * 4);
                wr32(entries + e * 4,
                     static_cast<uint32_t>(int64_t(off) + delta));
            }
        } else if (fourcc(type, kCo64) && payloadLen >= 8) {
            uint32_t entryCount = rd32(payload + 4);
            uint8_t* entries = payload + 8;
            for (uint32_t e = 0; e < entryCount; ++e) {
                uint64_t off = rd64(entries + e * 8);
                wr64(entries + e * 8, uint64_t(int64_t(off) + delta));
            }
        } else if (fourcc(type, "trak") || fourcc(type, "mdia") ||
                   fourcc(type, "minf") || fourcc(type, "stbl") ||
                   fourcc(type, "moov")) {
            // 这些是容器 box，进入递归（注意 moov 本身从顶层进入，
            // 但其 payload 前可能有 mvhd 等，统一递归即可）
            patchOffsets(payload, payloadLen, delta);
        }
        i += static_cast<size_t>(size);
    }
}

}  // namespace

bool Mp4Faststart::relocateBuffer(std::vector<uint8_t>& data) {
    const size_t len = data.size();
    size_t pos = 0;

    size_t ftypEnd = 0;
    size_t moovStart = 0, moovSize = 0, moovHeader = 0;
    bool foundFtyp = false, foundMoov = false;
    // 记录 moov 之前（除 ftyp）的 box 总长度 —— 这些 box 将被后移 moovSize
    uint64_t beforeMoov = 0;

    while (pos + 8 <= len) {
        uint64_t size = rd32(data.data() + pos);
        size_t hdr = 8;
        if (size == 1) {
            if (pos + 16 > len) break;
            size = rd64(data.data() + pos + 8);
            hdr = 16;
        } else if (size == 0) {
            size = len - pos;
        }
        if (size < hdr || pos + size > len) break;
        const uint8_t* type = data.data() + pos + 4;

        if (fourcc(type, "ftyp")) {
            foundFtyp = true;
            ftypEnd = pos + static_cast<size_t>(size);
        } else if (fourcc(type, kMoov)) {
            foundMoov = true;
            moovStart = pos;
            moovSize = static_cast<size_t>(size);
            moovHeader = hdr;
            break;  // 找到 moov 即可；其后 box 不受影响
        } else {
            // moov 之前的非 ftyp box（通常是 mdat）
            beforeMoov += size;
        }
        pos += static_cast<size_t>(size);
    }

    if (!foundMoov) return false;  // 没有 moov，无需处理
    if (!foundFtyp) ftypEnd = 0;   // 没有 ftyp 就插到最前面

    // 如果 moov 已经在 ftyp 之后且前面没有别的 box，则无需移动
    if (moovStart <= ftypEnd) return true;

    // delta: mdat 等原在 ftyp 与 moov 之间的 box 将整体后移 moovSize
    int64_t delta = static_cast<int64_t>(moovSize);

    // 取出 moov 字节并 patch 其中的 stco/co64
    std::vector<uint8_t> moov(data.begin() + moovStart,
                              data.begin() + moovStart + moovSize);
    patchOffsets(moov.data() + moovHeader, moov.size() - moovHeader, delta);

    // 重组：[0..ftypEnd) + moov + [ftypEnd..moovStart) + [moovStart+moovSize..)
    std::vector<uint8_t> out;
    out.reserve(len);
    out.insert(out.end(), data.begin(), data.begin() + ftypEnd);
    out.insert(out.end(), moov.begin(), moov.end());
    out.insert(out.end(), data.begin() + ftypEnd, data.begin() + moovStart);
    out.insert(out.end(), data.begin() + moovStart + moovSize, data.end());
    data.swap(out);
    return true;
}

bool Mp4Faststart::relocate(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("Mp4Faststart: open %s failed: %s", path.c_str(),
                  std::strerror(errno));
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(st.st_size));
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = ::read(fd, buf.data() + off, buf.size() - off);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(n);
    }
    ::close(fd);

    if (!relocateBuffer(buf)) return false;

    // 写回临时文件再 rename，保证原子性
    std::string tmp = path + ".tmp";
    int wfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        LOG_ERROR("Mp4Faststart: write open %s failed: %s", tmp.c_str(),
                  std::strerror(errno));
        return false;
    }
    size_t woff = 0;
    while (woff < buf.size()) {
        ssize_t n = ::write(wfd, buf.data() + woff, buf.size() - woff);
        if (n <= 0) {
            ::close(wfd);
            ::unlink(tmp.c_str());
            return false;
        }
        woff += static_cast<size_t>(n);
    }
    ::close(wfd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        LOG_ERROR("Mp4Faststart: rename failed: %s", std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }
    LOG_INFO("Mp4Faststart: moov relocated for %s", path.c_str());
    return true;
}

}  // namespace streaming
