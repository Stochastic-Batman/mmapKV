#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>

namespace mmapkv {

// Owns a file descriptor and a MAP_SHARED read/write mapping of the whole file.
// Move-only. The destructor unmaps and closes.
// A zero-length file is valid, but mmap cannot map zero bytes, so data() is nullptr until the file is grown above zero.
class MappedFile {
public:
    // Opens the file, creating it if it does not exist.
    [[nodiscard]] static std::optional<MappedFile> open(const std::filesystem::path& path);

    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // Extends the file to new_size bytes and remaps it. 
    // Does nothing if new_size is not larger than the current size. Returns false on failure.
    // Invalidates every span or pointer obtained from bytes() before the call.
    [[nodiscard]] bool grow(std::size_t new_size);

    // Blocks until dirty pages of the mapping are written to disk.
    [[nodiscard]] bool sync();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<std::byte> bytes() noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

private:
    MappedFile(int fd, std::byte* data, std::size_t size) noexcept;
    void release() noexcept;

    int fd_ = -1;
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace mmapkv
