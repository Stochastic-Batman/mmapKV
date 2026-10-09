#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace mmapkv {

// On-disk record: | key_len u32 | val_len u32 | crc u32 | key bytes | value bytes |
// Integers are little-endian. val_len == kTombstone marks a deletion and has no value bytes.
// The CRC-32C covers key_len, val_len, the key and the value, but not the crc field itself.
inline constexpr std::uint32_t kTombstone = 0xFFFFFFFFu;
inline constexpr std::size_t kHeaderSize = 12;

struct DecodedRecord {
    std::string_view key;                   // points into the input buffer
    std::optional<std::string_view> value;  // nullopt for a tombstone; points into the input buffer
    std::size_t total_size;                 // bytes this record occupies on disk
};

// Number of bytes encode_record will write for these arguments.
std::size_t encoded_size(std::string_view key, std::optional<std::string_view> value) noexcept;

// Writes the record into out, which must hold at least encoded_size(key, value) bytes.
// The key must not be empty. Returns the number of bytes written.
std::size_t encode_record(std::span<std::byte> out, std::string_view key, std::optional<std::string_view> value) noexcept;

// Parses one record from the start of in. Returns nullopt if the bytes are truncated,
// the key is empty, the lengths don't fit in in, or the checksum doesn't match.
std::optional<DecodedRecord> decode_record(std::span<const std::byte> in) noexcept;

}  // namespace mmapkv
