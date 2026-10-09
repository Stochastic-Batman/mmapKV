#include "record.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::vector<std::byte> encode(std::string_view key, std::optional<std::string_view> value) {
    std::vector<std::byte> buf(mmapkv::encoded_size(key, value));
    const std::size_t written = mmapkv::encode_record(buf, key, value);
    expect(written == buf.size(), "encode_record writes exactly encoded_size bytes");
    return buf;
}

}  // namespace

int main() {
    using mmapkv::decode_record;

    // Round trip: a normal put.
    {
        const auto buf = encode("key", "value");
        const auto rec = decode_record(buf);
        expect(rec.has_value(), "put: decodes");
        if (rec) {
            expect(rec->key == "key", "put: key");
            expect(rec->value.has_value() && *rec->value == "value", "put: value");
            expect(rec->total_size == buf.size(), "put: total_size");
        }
    }

    // Round trip: a tombstone has no value.
    {
        const auto buf = encode("key", std::nullopt);
        const auto rec = decode_record(buf);
        expect(rec.has_value(), "tombstone: decodes");
        if (rec) {
            expect(rec->key == "key", "tombstone: key");
            expect(!rec->value.has_value(), "tombstone: no value");
            expect(rec->total_size == buf.size(), "tombstone: total_size");
        }
    }

    // An empty value is a real value, not a tombstone.
    {
        const auto buf = encode("key", std::string_view{});
        const auto rec = decode_record(buf);
        expect(rec.has_value(), "empty value: decodes");
        if (rec) {
            expect(rec->value.has_value() && rec->value->empty(), "empty value: is not a tombstone");
        }
    }

    // Exact layout: sizes and little-endian length fields.
    {
        const auto buf = encode("abc", "12345");
        expect(buf.size() == mmapkv::kHeaderSize + 3 + 5, "layout: total size");

        std::uint32_t key_len = 0;
        std::uint32_t val_len = 0;
        std::memcpy(&key_len, buf.data(), 4);
        std::memcpy(&val_len, buf.data() + 4, 4);
        expect(key_len == 3, "layout: key_len");
        expect(val_len == 5, "layout: val_len");
        expect(buf[0] == std::byte{3} && buf[1] == std::byte{0}, "layout: key_len is little-endian");
        expect(std::memcmp(buf.data() + 12, "abc12345", 8) == 0, "layout: key then value bytes");
    }

    // Any single flipped bit, anywhere in the record, makes decoding fail.
    {
        const auto original = encode("key", "value");
        for (std::size_t i = 0; i < original.size(); ++i) {
            for (const std::uint8_t mask : {0x01, 0x80}) {
                auto copy = original;
                copy[i] ^= static_cast<std::byte>(mask);
                expect(!decode_record(copy).has_value(), "corruption is detected");
            }
        }
    }

    // A record cut short at any point fails to decode.
    {
        const auto original = encode("key", "value");
        for (std::size_t cut = 0; cut < original.size(); ++cut) {
            const std::span<const std::byte> prefix(original.data(), cut);
            expect(!decode_record(prefix).has_value(), "truncation is detected");
        }
    }

    // Zeroed space (the end of the log) is not a record.
    {
        const std::vector<std::byte> zeros(64);
        expect(!decode_record(zeros).has_value(), "all-zero bytes: end of log");
    }

    // Lengths larger than the buffer must be rejected, not read.
    {
        std::vector<std::byte> buf(mmapkv::kHeaderSize);
        const std::uint32_t huge = 0xFFFFFFFEu;
        std::memcpy(buf.data(), &huge, 4);
        std::memcpy(buf.data() + 4, &huge, 4);
        expect(!decode_record(buf).has_value(), "huge lengths are rejected");

        const std::uint32_t tombstone = mmapkv::kTombstone;
        std::memcpy(buf.data(), &tombstone, 4);
        expect(!decode_record(buf).has_value(), "huge key_len with tombstone is rejected");
    }

    // Records can be read back to back using total_size, with trailing bytes ignored.
    {
        const auto first = encode("a", "1");
        const auto second = encode("bb", std::nullopt);

        std::vector<std::byte> log;
        log.insert(log.end(), first.begin(), first.end());
        log.insert(log.end(), second.begin(), second.end());
        log.insert(log.end(), 20, std::byte{0});

        const auto r1 = decode_record(log);
        expect(r1.has_value() && r1->key == "a", "sequence: first record");
        if (r1) {
            const auto r2 = decode_record(std::span<const std::byte>(log).subspan(r1->total_size));
            expect(r2.has_value() && r2->key == "bb" && !r2->value.has_value(),
                   "sequence: second record");
            if (r2) {
                const std::size_t end = r1->total_size + r2->total_size;
                expect(!decode_record(std::span<const std::byte>(log).subspan(end)).has_value(),
                       "sequence: log ends at the zeroed tail");
            }
        }
    }

    return failures == 0 ? 0 : 1;
}
