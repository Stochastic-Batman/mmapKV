#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mmapkv {

// CRC-32C (Castagnoli). Check value: crc32c("123456789") == 0xE3069283.
//
// Pass the result of a previous call as `seed` to continue a checksum across several spans,
// so the record CRC can cover header fields, key and value without copying them into one buffer.
std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed = 0) noexcept;
}  // namespace mmapkv
