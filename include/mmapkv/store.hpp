#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mmapkv {

struct Options {
    // If true, every put and remove is flushed to disk before it returns.
    // Safest, but much slower. If false, call Store::sync() at your own checkpoints.
    bool sync_on_write = false;
};

// An append-only key-value store backed by a single memory-mapped file.
//
// Keys must not be empty. Keys and values must each be smaller than 4 GiB - 1 byte.
// A Store is not thread-safe, and only one Store may have a given file open at a time.
class Store {
public:
    // Opens the store at `path`, creating it if the file does not exist or is empty.
    // Returns nullopt if the file cannot be opened or mapped, or if it is not a
    // mmapKV file (wrong magic) or has an unsupported format version.
    // A damaged tail left by a crash is discarded, not treated as an error.
    [[nodiscard]] static std::optional<Store> open(const std::filesystem::path& path, Options options = {});

    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;

    // Stores `value` under `key`, replacing any previous value.
    // Returns false if the key is empty, a size limit is exceeded, or writing fails.
    [[nodiscard]] bool put(std::string_view key, std::string_view value);

    // Returns a copy of the value, or nullopt if the key is absent.
    // An empty value is a real value and comes back as an empty string.
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;

    // Deletes `key`. Deleting an absent key succeeds and writes nothing.
    // Returns false only for an empty key or a failed write.
    [[nodiscard]] bool remove(std::string_view key);

    // Flushes all writes to disk and blocks until they are durable.
    [[nodiscard]] bool sync();

    // Number of live keys.
    [[nodiscard]] std::size_t size() const noexcept;

    // All live keys, in unspecified order.
    [[nodiscard]] std::vector<std::string> keys() const;

private:
    struct Impl;
    explicit Store(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace mmapkv
