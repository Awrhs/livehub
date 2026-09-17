#include "cache/FileCache.hpp"
#include "cache/LruCache.hpp"
#include "core/Logger.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

namespace streaming {

MappedFile::~MappedFile() {
    if (valid()) {
        ::munmap(const_cast<char*>(data_), size_);
    }
}

std::shared_ptr<MappedFile> MappedFile::open(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        LOG_DEBUG("MappedFile::open %s failed: %s", path.c_str(),
                  strerror(errno));
        return nullptr;
    }

    struct stat st{};
    if (::fstat(fd, &st) < 0 || st.st_size == 0) {
        ::close(fd);
        return nullptr;
    }

    void* p = ::mmap(nullptr, static_cast<size_t>(st.st_size),
                     PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);  // mmap 后即可关闭 fd
    if (p == MAP_FAILED) {
        LOG_ERROR("mmap %s failed: %s", path.c_str(), strerror(errno));
        return nullptr;
    }
    return std::shared_ptr<MappedFile>(
        new MappedFile(static_cast<const char*>(p),
                       static_cast<size_t>(st.st_size)));
}

struct FileCache::Impl {
    explicit Impl(size_t maxEntries) : cache(maxEntries, 0, nullptr) {}
    LruCache<std::string, std::shared_ptr<MappedFile>> cache;
};

FileCache::FileCache(size_t maxEntries)
    : impl_(new Impl(maxEntries)) {}

std::shared_ptr<MappedFile> FileCache::get(const std::string& path) {
    auto cached = impl_->cache.get(path);
    if (cached) return *cached;

    auto mapped = MappedFile::open(path);
    if (mapped) {
        impl_->cache.put(path, mapped);
    }
    return mapped;
}

void FileCache::erase(const std::string& path) {
    impl_->cache.erase(path);
}

size_t FileCache::size() const {
    return impl_->cache.size();
}

}  // namespace streaming
