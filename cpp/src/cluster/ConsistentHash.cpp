#include "cluster/ConsistentHash.hpp"

#include <cstring>

namespace streaming {

namespace {

// 自包含 MD5 实现（RFC 1321），避免对 OpenSSL 的硬依赖。
struct Md5 {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe,
             d0 = 0x10325476;
    uint64_t totalLen = 0;
    uint8_t block[64];
    int blockLen = 0;

    static uint32_t leftRotate(uint32_t x, int c) {
        return (x << c) | (x >> (32 - c));
    }
    void processBlock(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
            0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
            0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
            0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
            0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
            0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
            0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
            0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
            0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            M[i] = static_cast<uint32_t>(p[i * 4]) |
                   (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D;
            D = C;
            C = B;
            B = B + leftRotate(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    void update(const uint8_t* data, size_t len) {
        totalLen += len;
        while (len > 0) {
            size_t copy = 64 - blockLen;
            if (copy > len) copy = len;
            std::memcpy(block + blockLen, data, copy);
            blockLen += static_cast<int>(copy);
            data += copy;
            len -= copy;
            if (blockLen == 64) {
                processBlock(block);
                blockLen = 0;
            }
        }
    }
    std::string finalize() {
        uint64_t bitLen = totalLen * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (blockLen != 56) update(&zero, 1);
        uint8_t lenBuf[8];
        for (int i = 0; i < 8; ++i) {
            lenBuf[i] = static_cast<uint8_t>(bitLen >> (i * 8));
        }
        update(lenBuf, 8);
        char out[16];
        uint32_t vals[4] = {a0, b0, c0, d0};
        for (int i = 0; i < 4; ++i) {
            out[i * 4] = vals[i] & 0xff;
            out[i * 4 + 1] = (vals[i] >> 8) & 0xff;
            out[i * 4 + 2] = (vals[i] >> 16) & 0xff;
            out[i * 4 + 3] = (vals[i] >> 24) & 0xff;
        }
        return std::string(out, 16);
    }
};

}  // namespace

std::string ConsistentHash::md5(const std::string& input) {
    Md5 m;
    m.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    return m.finalize();
}

uint32_t ConsistentHash::hashKey(const std::string& key) {
    std::string d = md5(key);
    return static_cast<uint8_t>(d[0]) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d[3])) << 24);
}

void ConsistentHash::addNode(const std::string& node) {
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < vnodes_; ++i) {
        uint32_t h = hashKey(node + "#" + std::to_string(i));
        ring_[h] = node;
    }
}

void ConsistentHash::removeNode(const std::string& node) {
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < vnodes_; ++i) {
        uint32_t h = hashKey(node + "#" + std::to_string(i));
        ring_.erase(h);
    }
}

void ConsistentHash::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    ring_.clear();
}

std::string ConsistentHash::getNode(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (ring_.empty()) return "";
    uint32_t h = hashKey(key);
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) it = ring_.begin();
    return it->second;
}

std::map<std::string, int> ConsistentHash::distribution(
    const std::vector<std::string>& keys) const {
    std::map<std::string, int> result;
    for (const auto& k : keys) {
        std::string n = getNode(k);
        if (!n.empty()) ++result[n];
    }
    return result;
}

size_t ConsistentHash::nodeCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, int> uniq;
    for (const auto& [h, n] : ring_) uniq[n]++;
    return uniq.size();
}

size_t ConsistentHash::ringSize() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ring_.size();
}

std::map<uint32_t, std::string> ConsistentHash::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ring_;
}

}  // namespace streaming
