#include "crc32c.hpp"

#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::span<const std::byte> bytes(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}

}  // namespace

int main() {
    using mmapkv::crc32c;
    constexpr std::string_view check = "123456789";
    constexpr std::uint32_t expected = 0xE3069283u;

    expect(crc32c(bytes(check)) == expected, "check value");
    expect(crc32c(std::span<const std::byte>{}) == 0u, "empty input");

    for (std::size_t cut = 0; cut <= check.size(); ++cut) {
        std::uint32_t first = crc32c(bytes(check.substr(0, cut)));
        std::uint32_t chained = crc32c(bytes(check.substr(cut)), first);
        expect(chained == expected, "split chaining");
    }

    return failures == 0 ? 0 : 1;
}
