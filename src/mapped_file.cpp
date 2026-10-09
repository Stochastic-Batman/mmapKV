#include "mapped_file.hpp"

#include <cstddef>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>


namespace mmapkv {

std::optional<MappedFile> MappedFile::open(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        return std::nullopt;
    }

    struct stat info {};
    if (::fstat(fd, &info) == -1) {
        ::close(fd);
        return std::nullopt;
    }

    const std::size_t size = static_cast<std::size_t>(info.st_size);
    std::byte* data = nullptr;

    if (size > 0) {
        void* mapping = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) {
            ::close(fd);
            return std::nullopt;
        }
        data = static_cast<std::byte*>(mapping);
    }

    return MappedFile{fd, data, size};
}

MappedFile::MappedFile(int fd, std::byte* data, std::size_t size) noexcept : fd_(fd), data_(data), size_(size) {}

MappedFile::~MappedFile() {
    release();
}

MappedFile::MappedFile(MappedFile&& other) noexcept : fd_(std::exchange(other.fd_, -1)), data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = std::exchange(other.fd_, -1);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

bool MappedFile::grow(std::size_t new_size) {
    if (new_size <= size_) {
        return true;
    }

    if (::ftruncate(fd_, static_cast<off_t>(new_size)) == -1) {
        return false;
    }

    void* mapping = ::mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapping == MAP_FAILED) {
        return false;
    }

    if (data_ != nullptr) ::munmap(data_, size_);

    data_ = static_cast<std::byte*>(mapping);
    size_ = new_size;
    return true;
}

bool MappedFile::sync() {
    if (data_ != nullptr && ::msync(data_, size_, MS_SYNC) == -1) {
        return false;
    }
    return ::fsync(fd_) == 0;
}

std::size_t MappedFile::size() const noexcept {
    return size_;
}

std::span<std::byte> MappedFile::bytes() noexcept {
    return {data_, size_};
}

std::span<const std::byte> MappedFile::bytes() const noexcept {
    return {data_, size_};
}

void MappedFile::release() noexcept {
    if (data_ != nullptr) ::munmap(data_, size_);
    if (fd_ != -1) ::close(fd_);
    fd_ = -1;
    data_ = nullptr;
    size_ = 0;
}

}  // namespace mmapkv
