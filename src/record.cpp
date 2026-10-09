#include "record.hpp"

#include "crc32c.hpp"

#include <bit>
#include <cassert>
#include <cstring>
#include <type_traits>


namespace mmapkv {

namespace {

struct RecordHeader {
    std::uint32_t key_len;
    std::uint32_t val_len;
    std::uint32_t crc;
};

// The header is written to disk as-is, so the in-memory byte order is the file's byte order.
static_assert(std::endian::native == std::endian::little, "mmapKV's on-disk format is little-endian");
static_assert(sizeof(RecordHeader) == kHeaderSize, "RecordHeader must have no padding");
static_assert(std::is_trivially_copyable_v<RecordHeader>);

std::span<const std::byte> as_bytes(std::string_view s) noexcept {
    return std::as_bytes(std::span(s.data(), s.size()));
}

// CRC over key_len, val_len, key, value, in that order, without the crc field itself.
// For a tombstone, `value` is empty and val_len is kTombstone.
std::uint32_t checksum(std::uint32_t key_len, std::uint32_t val_len, std::string_view key, std::string_view value) noexcept {
    const std::uint32_t lengths[2] = {key_len, val_len};
    std::uint32_t crc = crc32c(std::as_bytes(std::span(lengths)));
    crc = crc32c(as_bytes(key), crc);
    crc = crc32c(as_bytes(value), crc);
    return crc;
}

}  // namespace

std::size_t encoded_size(std::string_view key, std::optional<std::string_view> value) noexcept {
    return kHeaderSize + key.size() + (value ? value->size() : 0);
}

std::size_t encode_record(std::span<std::byte> out, std::string_view key, std::optional<std::string_view> value) noexcept {
    const std::string_view val = value.value_or(std::string_view{});

    assert(!key.empty());
    assert(key.size() < kTombstone);
    assert(val.size() < kTombstone);
    assert(out.size() >= encoded_size(key, value));

    RecordHeader header{};
    header.key_len = static_cast<std::uint32_t>(key.size());
    header.val_len = value ? static_cast<std::uint32_t>(val.size()) : kTombstone;
    header.crc = checksum(header.key_len, header.val_len, key, val);

    std::byte* p = out.data();
    std::memcpy(p, &header, kHeaderSize);
    std::memcpy(p + kHeaderSize, key.data(), key.size());
    if (!val.empty()) {
        std::memcpy(p + kHeaderSize + key.size(), val.data(), val.size());
    }

    return kHeaderSize + key.size() + val.size();
}

std::optional<DecodedRecord> decode_record(std::span<const std::byte> in) noexcept {
    if (in.size() < kHeaderSize) {
        return std::nullopt;
    }

    RecordHeader header;
    std::memcpy(&header, in.data(), kHeaderSize);

    if (header.key_len == 0) {
        return std::nullopt;
    }

    const bool tombstone = header.val_len == kTombstone;
    const std::uint64_t val_bytes = tombstone ? 0 : header.val_len;

    // 64-bit arithmetic: two 32-bit lengths plus the header cannot overflow it.
    const std::uint64_t total = kHeaderSize + std::uint64_t{header.key_len} + val_bytes;
    if (total > in.size()) {
        return std::nullopt;
    }

    const char* body = reinterpret_cast<const char*>(in.data()) + kHeaderSize;
    const std::string_view key(body, header.key_len);
    const std::string_view val(body + header.key_len, static_cast<std::size_t>(val_bytes));

    if (checksum(header.key_len, header.val_len, key, val) != header.crc) {
        return std::nullopt;
    }

    DecodedRecord record;
    record.key = key;
    if (!tombstone) {
        record.value = val;
    }
    record.total_size = static_cast<std::size_t>(total);
    return record;
}

}  // namespace mmapkv
