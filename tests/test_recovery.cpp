#include <mmapkv/store.hpp>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using mmapkv::Store;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// A path in the temp directory that is removed before and after use.
struct TempFile {
    fs::path path;

    explicit TempFile(const std::string& name)
        : path(fs::temp_directory_path() / ("mmapkv_test_recovery_" + name + ".kv")) {
        fs::remove(path);
    }
    ~TempFile() { fs::remove(path); }
};

// On-disk layout: a 16-byte file header, then records of 12 + key + value bytes.
// With one-byte keys and values every put record is 14 bytes, so a, b, c, d start at 16, 30, 44, 58.
constexpr std::streamoff kFileHeaderSize = 16;
constexpr std::streamoff kRecordHeaderSize = 12;
constexpr std::streamoff kPutRecordSize = 14;

std::streamoff offset_of(int index) {
    return kFileHeaderSize + index * kPutRecordSize;
}

// Overwrites bytes in place, the way a torn or corrupted write would.
void patch(const fs::path& path, std::streamoff offset, const std::string& bytes) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(offset);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Damages the value byte of the put record at `index`, which makes its checksum fail.
void corrupt_record(const fs::path& path, int index) {
    patch(path, offset_of(index) + kRecordHeaderSize + 1, "X");
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_abcd(const fs::path& path) {
    std::optional<Store> store = Store::open(path);
    expect(store.has_value(), "setup: open");
    if (!store) {
        return;
    }
    expect(store->put("a", "1") && store->put("b", "2") && store->put("c", "3") && store->put("d", "4"),
           "setup: put a, b, c, d");
}

// Everything from a damaged record onward is dropped; everything before it survives.
void test_corrupt_record_drops_the_rest() {
    TempFile file("corrupt");
    write_abcd(file.path);
    corrupt_record(file.path, 2);

    std::optional<Store> store = Store::open(file.path);
    expect(store.has_value(), "corrupt: reopen succeeds");
    if (!store) {
        return;
    }
    expect(store->get("a") == "1" && store->get("b") == "2", "corrupt: records before the damage survive");
    expect(!store->get("c").has_value(), "corrupt: the damaged record is gone");
    expect(!store->get("d").has_value(), "corrupt: records after the damage are dropped too");
    expect(store->size() == 2, "corrupt: size");
}

// A valid record sitting behind a damaged one must not come back once new records are written
// over the gap. This is what zeroing the tail on open protects against.
void test_dropped_record_does_not_resurrect() {
    TempFile file("resurrect");
    write_abcd(file.path);
    corrupt_record(file.path, 2);

    {
        std::optional<Store> store = Store::open(file.path);
        // 14 bytes, so it lands exactly where c was and ends exactly where d starts.
        expect(store.has_value() && store->put("e", "5"), "resurrect: write over the damaged spot");
    }

    std::optional<Store> store = Store::open(file.path);
    expect(store.has_value(), "resurrect: reopen succeeds");
    if (!store) {
        return;
    }
    expect(store->get("a") == "1" && store->get("b") == "2" && store->get("e") == "5",
           "resurrect: good data is intact");
    expect(!store->get("c").has_value(), "resurrect: c stays gone");
    expect(!store->get("d").has_value(), "resurrect: d does not come back from behind the gap");
    expect(store->size() == 3, "resurrect: size");
}

// The file is cut off in the middle of a record.
void test_truncated_mid_record() {
    TempFile file("truncated");
    write_abcd(file.path);
    fs::resize_file(file.path, offset_of(2) + 5);

    {
        std::optional<Store> store = Store::open(file.path);
        expect(store.has_value(), "truncated: reopen succeeds");
        if (!store) {
            return;
        }
        expect(store->get("a") == "1" && store->get("b") == "2", "truncated: complete records survive");
        expect(!store->get("c").has_value(), "truncated: the cut record is gone");
        expect(store->put("z", "9"), "truncated: the file regrows on the next write");
    }

    std::optional<Store> store = Store::open(file.path);
    expect(store.has_value(), "truncated: second reopen succeeds");
    if (!store) {
        return;
    }
    expect(store->get("z") == "9" && store->get("a") == "1" && store->get("b") == "2",
           "truncated: state after more writes");
    expect(store->size() == 3, "truncated: size after more writes");
}

// The file is cut inside the first record, or right after the file header.
void test_truncated_early() {
    for (std::streamoff cut : {kFileHeaderSize, kFileHeaderSize + 4, kFileHeaderSize + kRecordHeaderSize}) {
        TempFile file("early");
        write_abcd(file.path);
        fs::resize_file(file.path, static_cast<std::uintmax_t>(cut));

        std::optional<Store> store = Store::open(file.path);
        expect(store.has_value(), "early: reopen succeeds");
        if (!store) {
            continue;
        }
        expect(store->size() == 0, "early: no record survives");
        expect(store->put("k", "v") && store->get("k") == "v", "early: the store is still usable");
    }
}

// Junk bytes written over the end of the log are ignored, and the log carries on after the last good record.
void test_garbage_after_the_last_record() {
    TempFile file("garbage");
    write_abcd(file.path);
    patch(file.path, offset_of(2), std::string(40, '\xAB'));

    {
        std::optional<Store> store = Store::open(file.path);
        expect(store.has_value(), "garbage: reopen succeeds");
        if (!store) {
            return;
        }
        expect(store->size() == 2, "garbage: only the records before it survive");
        expect(store->put("n", "7"), "garbage: new writes work");
    }

    std::optional<Store> store = Store::open(file.path);
    expect(store.has_value() && store->get("n") == "7" && store->get("a") == "1" && store->size() == 3,
           "garbage: new write survives another reopen");
}

// If the last record is a delete that was never completed, the delete did not happen.
void test_lost_delete_brings_the_key_back() {
    TempFile file("tombstone");
    {
        std::optional<Store> store = Store::open(file.path);
        expect(store.has_value() && store->put("a", "1") && store->remove("a"), "tombstone: setup");
    }
    // The tombstone starts at 30 and is 12 + 1 bytes. Damage its key byte.
    patch(file.path, offset_of(1) + kRecordHeaderSize, "X");

    std::optional<Store> store = Store::open(file.path);
    expect(store.has_value(), "tombstone: reopen succeeds");
    if (!store) {
        return;
    }
    expect(store->get("a") == "1", "tombstone: the key is back, because the delete was never durable");
}

// Opening a damaged file repeatedly gives the same answer each time.
void test_recovery_is_repeatable() {
    TempFile file("repeat");
    write_abcd(file.path);
    corrupt_record(file.path, 1);

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::optional<Store> store = Store::open(file.path);
        expect(store.has_value(), "repeat: open succeeds");
        if (!store) {
            return;
        }
        expect(store->get("a") == "1" && !store->get("b").has_value() && store->size() == 1,
               "repeat: same result every time");
    }
}

// Opening and reading a damaged file must not change it. Only a write may discard what follows the damage.
void test_reading_does_not_modify_a_damaged_file() {
    TempFile file("readonly");
    write_abcd(file.path);
    corrupt_record(file.path, 1);
    const std::string before = read_file(file.path);

    {
        std::optional<Store> store = Store::open(file.path);
        expect(store.has_value(), "readonly: reopen succeeds");
        if (!store) {
            return;
        }
        expect(store->get("a") == "1" && !store->get("b").has_value() && !store->get("d").has_value(),
               "readonly: reads see only the records before the damage");
        expect(store->size() == 1 && store->keys().size() == 1, "readonly: size and keys");
    }

    expect(read_file(file.path) == before, "readonly: the file is byte-for-byte unchanged after reading");
}

// A file that was created and grown, but never got its header, is a new store.
void test_blank_file_is_a_new_store() {
    TempFile file("blank");
    std::ofstream(file.path, std::ios::binary).write(std::string(4096, '\0').data(), 4096);

    {
        std::optional<Store> store = Store::open(file.path);
        expect(store.has_value(), "blank: an all-zero file opens");
        expect(store.has_value() && store->size() == 0, "blank: and is empty");
        expect(store.has_value() && store->put("k", "v"), "blank: and accepts writes");
    }

    std::optional<Store> store = Store::open(file.path);
    expect(store.has_value() && store->get("k") == "v", "blank: and reopens as a normal store");
}

}  // namespace

int main() {
    test_corrupt_record_drops_the_rest();
    test_dropped_record_does_not_resurrect();
    test_truncated_mid_record();
    test_truncated_early();
    test_garbage_after_the_last_record();
    test_lost_delete_brings_the_key_back();
    test_recovery_is_repeatable();
    test_reading_does_not_modify_a_damaged_file();
    test_blank_file_is_a_new_store();

    return failures == 0 ? 0 : 1;
}
