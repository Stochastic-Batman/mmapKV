#include "crc32c.hpp"


namespace mmapkv {

// Per https://en.wikipedia.org/wiki/Cyclic_redundancy_check, CRC-32C is:
// x^32 + x^28 + x^27 + x^26 + x^25 + x^23 + x^22 + x^20 + x^19 + x^18 + x^14 + x^13 + x^11 + x^10 + x^9 + x^8 + x^6 + 1
// Normal form: 0x1EDC6F41. The loop below shifts right and processes the least significant bit first,
// so it uses the reflected form, 0x82F63B78.
std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed) noexcept {
    constexpr std::uint32_t reflected = 0x82F63B78u;
    std::uint32_t crc = ~seed;

    for (std::byte b : data) {
	crc ^= std::to_integer<std::uint8_t>(b);

	for (short bit = 0; bit < 8; ++bit) {
	    crc = (crc >> 1) ^ ((crc & 1u) ? reflected : 0u);
	}
    }

    return crc ^ 0xFFFFFFFFu;
}

} // namespace mmapkv
