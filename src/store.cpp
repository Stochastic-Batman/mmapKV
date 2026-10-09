#include <mmapkv/store.hpp>

#include "mapped_file.hpp"
#include "record.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <unordered_map>
#include <utility>

namespace mmapkv {

// The version field is written to disk as-is, so the in-memory byte order is the file's byte order.
static_assert(std::endian::native == std::endian::little, "mmapKV's on-disk format is little-endian");

namespace {

// File header, 16 bytes at offset 0:
//   bytes 0-7   magic: ASCII "mmapKV" followed by two zero bytes
//   bytes 8-11  format version, u32 little-endian
//   bytes 12-15 reserved, zero
// Records start right after it.
constexpr std::size_t kFileHeaderSize = 16;
constexpr char kMagic[8] = {'m', 'm', 'a', 'p', 'K', 'V', '\0', '\0'};
constexpr std::uint32_t kVersion = 1;

// A new file is created at this size and grown by doubling whenever a record does not fit.
constexpr std::size_t kInitialSize = 4096;

// Where a live key's value sits in the file. These are offsets, never pointers:
// grow() remaps the file and may move it, which would leave a pointer dangling but keeps an offset valid.
struct IndexEntry {
    std::size_t value_offset;
    std::size_t value_len;
};

// So index.find(std::string_view) works without building a std::string.
struct StringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view s) const noexcept { 
	return std::hash<std::string_view>{}(s); 
    }
};

using Index = std::unordered_map<std::string, IndexEntry, StringHash, std::equal_to<>>;

enum class HeaderState {
    Valid,    // magic and version match
    Blank,    // all zero: a file that was created but never initialised
    Invalid,  // too short, wrong magic, or an unsupported version
};

HeaderState inspect_file_header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kFileHeaderSize) {
        return HeaderState::Invalid;
    }

    const std::span<const std::byte> header = bytes.first(kFileHeaderSize);
    if (std::all_of(header.begin(), header.end(), [](std::byte b) { return b == std::byte{0}; })) {
        return HeaderState::Blank;
    }

    std::uint32_t version = 0;
    std::memcpy(&version, header.data() + sizeof(kMagic), sizeof(version));
    const bool valid = std::memcmp(header.data(), kMagic, sizeof(kMagic)) == 0 && version == kVersion;

    return valid ? HeaderState::Valid : HeaderState::Invalid;
}

void write_file_header(std::span<std::byte> bytes) noexcept {
    std::memcpy(bytes.data(), kMagic, sizeof(kMagic));
    std::memcpy(bytes.data() + sizeof(kMagic), &kVersion, sizeof(kVersion));
}

}  // namespace


struct Store::Impl {
    MappedFile file;
    Index index;
    std::size_t end = kFileHeaderSize;
    Options options;

    Impl(MappedFile f, Options o) : file(std::move(f)), options(o) {}

    void set(std::string_view key, IndexEntry entry) {
        if (Index::iterator it = index.find(key); it != index.end()) {
            it->second = entry;
        } else {
            index.emplace(std::string(key), entry);
        }
    }

    void erase(std::string_view key) {
	if (Index::iterator it = index.find(key); it != index.end()) {
            index.erase(it);
        }
    }

    // Rebuilds the index from the log and finds where the log ends. Called once, from open().
    void scan() {
        const std::span<std::byte> bytes = file.bytes();
        std::size_t pos = kFileHeaderSize;

        // decode_record fails on a torn write, on corruption, and on the zeroed space after the last record alike, so the end of the log needs no special case.
        while (pos < bytes.size()) {
	    const std::optional<DecodedRecord> record = decode_record(bytes.subspan(pos));
            if (!record) {
                break;
            }

            if (record->value) set(record->key, {pos + kHeaderSize + record->key.size(), record->value->size()});
            else erase(record->key);
            
	    pos += record->total_size;
        }

        end = pos;

        // Everything after the last good record is junk from a torn write or a stale one. 
	// Zero it, so a later record can never run into a leftover record that was hidden behind a torn one.
        std::memset(bytes.data() + pos, 0, bytes.size() - pos);
    }

    // The shared write path for put (value) and remove (nullopt).
    bool append(std::string_view key, std::optional<std::string_view> value) {
        const std::size_t need = encoded_size(key, value);

        if (end + need > file.size()) {
            if (!file.grow(std::max(file.size() * 2, end + need))) {
                return false; 
            }
        }

        // Fetch the span after the grow, which may have moved the mapping.
        encode_record(file.bytes().subspan(end), key, value);

        if (value) {
            set(key, {end + kHeaderSize + key.size(), value->size()});
        } else {
            erase(key);
        }
        end += need;

        return !options.sync_on_write || file.sync();
    }
};

std::optional<Store> Store::open(const std::filesystem::path& path, Options options) {
    std::optional<MappedFile> file = MappedFile::open(path);
    if (!file) {
        return std::nullopt;
    }

    // A brand-new file is empty. Grow it, and it becomes a blank file like any other fresh one.
    if (file->size() == 0 && !file->grow(kInitialSize)) {
        return std::nullopt;
    }

    switch (inspect_file_header(file->bytes())) {
        case HeaderState::Valid:
            break;
        case HeaderState::Blank:
            // Also covers a crash between growing a new file and writing its header.
            write_file_header(file->bytes());
            break;
        case HeaderState::Invalid:
            return std::nullopt;
    }

    std::unique_ptr<Impl> impl = std::make_unique<Impl>(std::move(*file), options);
    impl->scan();
    return Store(std::move(impl));
}

// These have to live here, where Impl is a complete type.
Store::Store(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

bool Store::put(std::string_view key, std::string_view value) {
    if (key.empty() || key.size() >= kTombstone || value.size() >= kTombstone) {
        return false;
    }
    return impl_->append(key, value);
}

std::optional<std::string> Store::get(std::string_view key) const {
    const Index::iterator it = impl_->index.find(key);
    if (it == impl_->index.end()) {
        return std::nullopt;
    }

    const std::span<const std::byte> bytes = impl_->file.bytes();
    const char* value = reinterpret_cast<const char*>(bytes.data()) + it->second.value_offset;
    return std::string(value, it->second.value_len);
}

bool Store::remove(std::string_view key) {
    if (key.empty()) {
        return false;
    }
    if (impl_->index.find(key) == impl_->index.end()) {
        return true;  // nothing to delete, and nothing to write
    }
    return impl_->append(key, std::nullopt);
}

bool Store::sync() {
    return impl_->file.sync();
}

std::size_t Store::size() const noexcept {
    return impl_->index.size();
}

std::vector<std::string> Store::keys() const {
    std::vector<std::string> keys;
    keys.reserve(impl_->index.size());
    for (const Index::value_type& entry : impl_->index) {
        keys.push_back(entry.first);
    }
    return keys;
}

}  // namespace mmapkv
