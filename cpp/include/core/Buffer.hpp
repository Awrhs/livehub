#pragma once

#include "core/NonCopyable.hpp"

#include <algorithm>
#include <cstring>
#include <endian.h>
#include <string>
#include <vector>

namespace streaming {

/// 一个可扩容的缓冲区，类似 muduo 的 Buffer。
/// 内存布局：
///   +-------------------+------------------+------------------+
///   | prependableBytes  |  readableBytes   |  writableBytes   |
///   +-------------------+------------------+------------------+
///   0                   readerIndex        writerIndex        size
///
/// 设计要点：
/// - prependable 区域用于在数据前插入长度等头部信息（无需移动数据）
/// - readv + extrabuf：栈上额外缓冲，readv 一次性读，减少系统调用
class Buffer : NonCopyable {
public:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitialSize = 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    size_t readableBytes() const {
        return writerIndex_ - readerIndex_;
    }
    size_t writableBytes() const {
        return buffer_.size() - writerIndex_;
    }
    size_t prependableBytes() const {
        return readerIndex_;
    }

    const char* peek() const { return begin() + readerIndex_; }

    /// 可读区转换为 string 并移动 readerIndex
    std::string retrieveAsString(size_t len) {
        // 防御：len 不能超过可读
        if (len > readableBytes()) len = readableBytes();
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }
    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }

    void retrieve(size_t len) {
        if (len < readableBytes()) {
            readerIndex_ += len;
        } else {
            // 全部读完，回到初始位置
            retrieveAll();
        }
    }

    /// 读直到 end 指针（end 必须在可读区内）
    void retrieveUntil(const char* end) {
        const char* start = peek();
        if (end > start && end <= beginWrite()) {
            retrieve(static_cast<size_t>(end - start));
        }
    }
    void retrieveAll() {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    /// 写入数据
    void append(const char* data, size_t len) {
        ensureWritable(len);
        std::memcpy(beginWrite(), data, len);
        writerIndex_ += len;
    }
    void append(const std::string& s) {
        append(s.data(), s.size());
    }

    char* beginWrite() { return begin() + writerIndex_; }
    const char* beginWrite() const { return begin() + writerIndex_; }

    void hasWritten(size_t len) { writerIndex_ += len; }

    void ensureWritable(size_t len) {
        if (writableBytes() < len) {
            makeSpace(len);
        }
    }

    /// 从 fd 读取，返回读取的字节数，errno 通过参数返回
    /// 使用 readv + 栈上 extrabuf，一次系统调用读取尽可能多数据
    ssize_t readFd(int fd, int* savedErrno);

    /// 写入到 fd
    ssize_t writeFd(int fd, int* savedErrno);

    /// 在数据前面 prepend（用于加长度头）
    void prepend(const void* data, size_t len) {
        if (len <= prependableBytes()) {
            // 前置预留区足够，直接前移读指针并拷贝
            readerIndex_ -= len;
            std::memcpy(begin() + readerIndex_, data, len);
            return;
        }
        // 预留区不够：把 [readerIndex_, writerIndex_) 的可读数据整体右移，
        // 在前面腾出 len 字节。vector 扩容时旧指针会失效，故先记录长度。
        const size_t readable = readableBytes();
        const size_t needed = writerIndex_ + (len - prependableBytes());
        buffer_.resize(needed);
        auto* p = begin();
        std::memmove(p + readerIndex_ + len, p + readerIndex_, readable);
        // readerIndex_ 保持不变：数据被移到 readerIndex_+len，新读指针即 readerIndex_
        writerIndex_ = readerIndex_ + len + readable;
        std::memcpy(p + readerIndex_, data, len);
    }

    /// 寻找 "\r\n" 位置（HTTP/RTMP 解析用）
    const char* findCRLF() const {
        const char* crlf = std::search(peek(), beginWrite(), CRLF, CRLF + 2);
        return crlf == beginWrite() ? nullptr : crlf;
    }

    /// 大端写入/读取（网络字节序）用于协议
    void appendInt32(int32_t v) {
        int32_t be = htobe32(v);
        append(reinterpret_cast<const char*>(&be), sizeof(be));
    }
    int32_t readInt32() {
        int32_t be;
        if (readableBytes() < sizeof(be)) return 0;
        std::memcpy(&be, peek(), sizeof(be));
        retrieve(sizeof(be));
        return be32toh(be);
    }

    void appendInt16(int16_t v) {
        int16_t be = htobe16(v);
        append(reinterpret_cast<const char*>(&be), sizeof(be));
    }
    int16_t readInt16() {
        int16_t be;
        if (readableBytes() < sizeof(be)) return 0;
        std::memcpy(&be, peek(), sizeof(be));
        retrieve(sizeof(be));
        return be16toh(be);
    }

    void appendInt8(int8_t v) {
        append(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    int8_t readInt8() {
        int8_t v = 0;
        if (readableBytes() >= sizeof(v)) {
            std::memcpy(&v, peek(), sizeof(v));
            retrieve(sizeof(v));
        }
        return v;
    }

private:
    char* begin() { return &*buffer_.begin(); }
    const char* begin() const { return &*buffer_.begin(); }

    void makeSpace(size_t len) {
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
            // 整体空间不足，扩容
            buffer_.resize(writerIndex_ + len);
        } else {
            // 复用前面空间：把可读数据移到前面
            size_t readable = readableBytes();
            std::memmove(begin() + kCheapPrepend, peek(), readable);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;

    static const char CRLF[];
};

}  // namespace streaming
