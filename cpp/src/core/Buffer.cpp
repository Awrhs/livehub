#include "core/Buffer.hpp"
#include "core/Logger.hpp"

#include <unistd.h>
#include <sys/uio.h>
#include <errno.h>

namespace streaming {

const char Buffer::CRLF[] = {'\r', '\n'};

ssize_t Buffer::readFd(int fd, int* savedErrno) {
    // 栈上额外缓冲：readv 一次读两块，避免多次 read
    char extrabuf[65536];
    struct iovec vec[2];
    const size_t writable = writableBytes();

    // 第一块：buffer_ 的可写区
    vec[0].iov_base = beginWrite();
    vec[0].iov_len = writable;
    // 第二块：栈上 extrabuf
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    // 若 buffer_ 可写区已够大，只用一块
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<size_t>(n) <= writable) {
        // 全部读入 buffer_
        writerIndex_ += static_cast<size_t>(n);
    } else {
        // buffer_ 满了，溢出到 extrabuf，追加进 buffer_
        writerIndex_ = buffer_.size();
        append(extrabuf, static_cast<size_t>(n) - writable);
    }
    return n;
}

ssize_t Buffer::writeFd(int fd, int* savedErrno) {
    const ssize_t n = ::write(fd, peek(), readableBytes());
    if (n < 0) {
        *savedErrno = errno;
    } else {
        retrieve(static_cast<size_t>(n));
    }
    return n;
}

}  // namespace streaming
