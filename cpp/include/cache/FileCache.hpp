#pragma once

#include "core/NonCopyable.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace streaming {

/**
 * MappedFile: 对一个文件的 mmap 映射（RAII）。
 *
 * 用 MAP_PRIVATE 只读映射，避免 read() 拷贝，适合发送大 TS 分片。
 * 析构自动 munmap。
 */
class MappedFile : NonCopyable {
public:
    ~MappedFile();

    static std::shared_ptr<MappedFile> open(const std::string& path);

    const char* data() const { return data_; }
    size_t size() const { return size_; }
    bool valid() const { return data_ != MAP_FAILED && data_ != nullptr; }

private:
    MappedFile(const char* data, size_t size) : data_(data), size_(size) {}
    const char* data_;
    size_t size_;
};

/**
 * FileCache: 对 mmap 文件做 LRU 回收，避免重复 open/mmap。
 */
class FileCache : NonCopyable {
public:
    explicit FileCache(size_t maxEntries = 256);

    /// 获取已映射文件；未命中则打开并缓存。失败返回 nullptr。
    std::shared_ptr<MappedFile> get(const std::string& path);

    void erase(const std::string& path);
    size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace streaming
