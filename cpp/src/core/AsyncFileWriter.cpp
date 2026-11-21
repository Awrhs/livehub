#include "core/AsyncFileWriter.hpp"

#include "core/Logger.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace streaming {

AsyncFileWriter::AsyncFileWriter(size_t maxQueueBytes)
    : maxQueueBytes_(maxQueueBytes) {}

AsyncFileWriter::~AsyncFileWriter() {
    stop();
}

void AsyncFileWriter::start() {
    if (running_.exchange(true)) return;
    stopping_ = false;
    worker_ = std::thread([this] { run(); });
}

void AsyncFileWriter::stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

uint64_t AsyncFileWriter::open(const std::string& path, bool append) {
    uint64_t id = nextId_.fetch_add(1);
    Op op;
    op.type = OpType::kOpen;
    op.id = id;
    op.path = path;
    op.appendMode = append;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(op));
    }
    cv_.notify_one();
    return id;
}

void AsyncFileWriter::append(uint64_t id, const uint8_t* data, size_t len) {
    if (len == 0) return;
    // 队列上限：磁盘跟不上时丢弃写入（宁可损坏分片也不能无界占用内存导致 OOM）
    if (maxQueueBytes_ > 0 && pendingBytes_.load() + len > maxQueueBytes_) {
        uint64_t n = droppedBytes_.fetch_add(len) + len;
        if (n == len || (n >> 24) != ((n - len) >> 24))  // 首次及每 16MB 告警一次
            LOG_WARN("AsyncFileWriter queue full (%zu bytes), dropped %llu bytes so far",
                     maxQueueBytes_, static_cast<unsigned long long>(n));
        return;
    }
    Op op;
    op.type = OpType::kAppend;
    op.id = id;
    op.data.assign(data, data + len);
    pendingBytes_.fetch_add(len);
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(op));
    }
    cv_.notify_one();
}

void AsyncFileWriter::fsync(uint64_t id) {
    Op op;
    op.type = OpType::kFsync;
    op.id = id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(op));
    }
    cv_.notify_one();
}

void AsyncFileWriter::close(uint64_t id, CloseCallback cb) {
    Op op;
    op.type = OpType::kClose;
    op.id = id;
    op.cb = std::move(cb);
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(op));
    }
    cv_.notify_one();
}

void AsyncFileWriter::unlink(const std::string& path) {
    Op op;
    op.type = OpType::kUnlink;
    op.path = path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(op));
    }
    cv_.notify_one();
}

void AsyncFileWriter::run() {
    for (;;) {
        Op op;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] {
                return stopping_.load() || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) break;
            op = std::move(queue_.front());
            queue_.pop_front();
        }
        apply(op);
    }
    // 排空：关闭所有仍打开的文件
    for (auto& kv : files_) {
        if (kv.second.fd >= 0) ::close(kv.second.fd);
    }
    files_.clear();
}

void AsyncFileWriter::apply(Op& op) {
    switch (op.type) {
        case OpType::kOpen: {
            int flags = O_WRONLY | O_CREAT;
            flags |= op.appendMode ? O_APPEND : O_TRUNC;
            int fd = ::open(op.path.c_str(), flags, 0644);
            if (fd < 0) {
                LOG_ERROR("AsyncFileWriter open %s failed: %s",
                          op.path.c_str(), strerror(errno));
                files_[op.id] = OpenFile{-1, 0};
            } else {
                files_[op.id] = OpenFile{fd, 0};
            }
            break;
        }
        case OpType::kAppend: {
            pendingBytes_.fetch_sub(op.data.size());
            auto it = files_.find(op.id);
            if (it == files_.end() || it->second.fd < 0) break;
            const uint8_t* p = op.data.data();
            size_t left = op.data.size();
            bool ok = true;
            while (left > 0) {
                ssize_t n = ::write(it->second.fd, p, left);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    LOG_ERROR("AsyncFileWriter write failed: %s",
                              strerror(errno));
                    ok = false;
                    break;
                }
                p += n;
                left -= static_cast<size_t>(n);
                it->second.written += static_cast<uint64_t>(n);
            }
            if (ok) totalWritten_.fetch_add(op.data.size());
            break;
        }
        case OpType::kFsync: {
            auto it = files_.find(op.id);
            if (it != files_.end() && it->second.fd >= 0) {
                ::fsync(it->second.fd);
            }
            break;
        }
        case OpType::kClose: {
            auto it = files_.find(op.id);
            bool ok = false;
            uint64_t written = 0;
            if (it != files_.end()) {
                written = it->second.written;
                if (it->second.fd >= 0) {
                    ok = (::close(it->second.fd) == 0);
                }
                files_.erase(it);
            }
            if (op.cb) op.cb(ok, written);
            break;
        }
        case OpType::kUnlink: {
            ::unlink(op.path.c_str());
            break;
        }
    }
}

}  // namespace streaming
