#include "mapped_file.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool starts_with_hello(std::span<const std::byte> s) {
    if (s.size() < 5) {
        return false;
    }
    return std::string_view(reinterpret_cast<const char*>(s.data()), 5) == "hello";
}

}  // namespace

int main() {
    using mmapkv::MappedFile;
    namespace fs = std::filesystem;

    const fs::path path = fs::temp_directory_path() / "mmapkv_test_mapped_file.bin";
    fs::remove(path);

    {
        auto file = MappedFile::open(path);
        expect(file.has_value(), "open creates a new file");
        if (!file) {
            return 1;
        }

        expect(file->size() == 0, "new file is empty");
        expect(file->bytes().empty(), "empty file has no bytes");

        expect(file->grow(4096), "grow to 4096");
        expect(file->size() == 4096, "size after grow");

        std::memcpy(file->bytes().data(), "hello", 5);
        expect(file->sync(), "sync succeeds");

        expect(file->grow(8192), "second grow");
        expect(file->size() == 8192, "size after second grow");
        expect(starts_with_hello(file->bytes()), "data survives remap");
    }

    {
        auto reopened = MappedFile::open(path);
        expect(reopened.has_value(), "reopen existing file");
        if (!reopened) {
            return 1;
        }

        expect(reopened->size() == 8192, "size persists across reopen");
        expect(starts_with_hello(reopened->bytes()), "data persists across reopen");

        MappedFile moved = std::move(*reopened);
        expect(moved.size() == 8192, "moved file keeps its size");
        expect(starts_with_hello(moved.bytes()), "moved file keeps its data");
    }

    expect(!MappedFile::open(fs::temp_directory_path() / "no_such_dir" / "x.bin").has_value(),
           "open fails in a missing directory");

    fs::remove(path);
    return failures == 0 ? 0 : 1;
}
