#include <mmapkv/store.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using mmapkv::Options;
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
        : path(fs::temp_directory_path() / ("mmapkv_test_store_" + name + ".kv")) {
        fs::remove(path);
    }
    ~TempFile() { fs::remove(path); }
};

std::string value_for(int i) {
    return "value-" + std::to_string(i) + std::string(100, 'x');
}

void test_basic_put_get() {
    TempFile f("basic");
    auto store = Store::open(f.path);
    expect(store.has_value(), "basic: open");
    if (!store) return;

    expect(store->size() == 0, "basic: new store is empty");
    expect(!store->get("missing").has_value(), "basic: missing key");

    expect(store->put("name", "mmapKV"), "basic: put");
    expect(store->get("name") == "mmapKV", "basic: get returns the value");
    expect(store->size() == 1, "basic: size after put");

    expect(store->put("name", "replaced"), "basic: overwrite");
    expect(store->get("name") == "replaced", "basic: latest value wins");
    expect(store->size() == 1, "basic: overwrite does not add a key");
}

void test_remove() {
    TempFile f("remove");
    auto store = Store::open(f.path);
    if (!store) { expect(false, "remove: open"); return; }

    expect(store->put("a", "1"), "remove: put a");
    expect(store->put("b", "2"), "remove: put b");
    expect(store->remove("a"), "remove: existing key");
    expect(!store->get("a").has_value(), "remove: key is gone");
    expect(store->get("b") == "2", "remove: other keys untouched");
    expect(store->size() == 1, "remove: size");

    expect(store->remove("a"), "remove: absent key is a successful no-op");
    expect(store->remove("never-existed"), "remove: unknown key is a successful no-op");
    expect(store->size() == 1, "remove: no-ops change nothing");

    expect(store->put("a", "again"), "remove: put after delete");
    expect(store->get("a") == "again", "remove: key can come back");
}

void test_invalid_input() {
    TempFile f("invalid");
    auto store = Store::open(f.path);
    if (!store) { expect(false, "invalid: open"); return; }

    expect(!store->put("", "value"), "invalid: empty key is rejected by put");
    expect(!store->remove(""), "invalid: empty key is rejected by remove");
    expect(!store->get("").has_value(), "invalid: empty key is never found");
    expect(store->size() == 0, "invalid: nothing was stored");

    expect(store->put("k", ""), "invalid: empty value is allowed");
    const auto v = store->get("k");
    expect(v.has_value() && v->empty(), "invalid: empty value is not a missing key");
}

void test_keys() {
    TempFile f("keys");
    auto store = Store::open(f.path);
    if (!store) { expect(false, "keys: open"); return; }

    expect(store->put("b", "2") && store->put("a", "1") && store->put("c", "3"), "keys: puts");
    expect(store->remove("c"), "keys: remove");
    auto keys = store->keys();
    std::sort(keys.begin(), keys.end());
    expect(keys == std::vector<std::string>({"a", "b"}), "keys: live keys only");
}

void test_persistence() {
    TempFile f("persist");
    {
        auto store = Store::open(f.path);
        if (!store) { expect(false, "persist: first open"); return; }
        expect(store->put("keep", "1"), "persist: put keep");
        expect(store->put("change", "old"), "persist: put change");
        expect(store->put("change", "new"), "persist: overwrite change");
        expect(store->put("drop", "x"), "persist: put drop");
        expect(store->remove("drop"), "persist: remove drop");
        expect(store->sync(), "persist: sync");
    }
    {
        auto store = Store::open(f.path);
        expect(store.has_value(), "persist: reopen");
        if (!store) return;
        expect(store->get("keep") == "1", "persist: value survives reopen");
        expect(store->get("change") == "new", "persist: latest overwrite survives");
        expect(!store->get("drop").has_value(), "persist: delete survives reopen");
        expect(store->size() == 2, "persist: size after reopen");

        // The store keeps working after a reopen, and appends after the old data.
        expect(store->put("later", "2"), "persist: put after reopen");
    }
    {
        auto store = Store::open(f.path);
        if (!store) { expect(false, "persist: third open"); return; }
        expect(store->get("later") == "2", "persist: write after reopen survives");
        expect(store->get("keep") == "1", "persist: old data still intact");
    }
}

// Enough data to force the file to grow, and remap, many times.
void test_growth() {
    TempFile f("growth");
    constexpr int kCount = 2000;
    {
        auto store = Store::open(f.path);
        if (!store) { expect(false, "growth: open"); return; }

        bool all_put = true;
        for (int i = 0; i < kCount; ++i) {
            all_put &= store->put("key-" + std::to_string(i), value_for(i));
        }
        expect(all_put, "growth: every put succeeds");

        bool all_found = true;
        for (int i = 0; i < kCount; ++i) {
            all_found &= store->get("key-" + std::to_string(i)) == value_for(i);
        }
        expect(all_found, "growth: values readable after many remaps");

        for (int i = 0; i < kCount; i += 3) {
            all_found &= store->remove("key-" + std::to_string(i));
        }
        expect(all_found, "growth: removals succeed");
    }
    {
        auto store = Store::open(f.path);
        if (!store) { expect(false, "growth: reopen"); return; }

        bool correct = true;
        std::size_t live = 0;
        for (int i = 0; i < kCount; ++i) {
            const auto v = store->get("key-" + std::to_string(i));
            if (i % 3 == 0) {
                correct &= !v.has_value();
            } else {
                correct &= v == value_for(i);
                ++live;
            }
        }
        expect(correct, "growth: contents correct after reopen");
        expect(store->size() == live, "growth: size after reopen");
    }
}

void test_large_value() {
    TempFile f("large");
    const std::string big(1 << 20, 'z');
    {
        auto store = Store::open(f.path);
        if (!store) { expect(false, "large: open"); return; }
        expect(store->put("big", big), "large: put 1 MiB value");
        expect(store->get("big") == big, "large: get 1 MiB value");
    }
    auto store = Store::open(f.path);
    expect(store.has_value() && store->get("big") == big, "large: 1 MiB value survives reopen");
}

void test_options_and_move() {
    TempFile f("options");
    auto opened = Store::open(f.path, Options{.sync_on_write = true});
    if (!opened) { expect(false, "options: open"); return; }

    expect(opened->put("k", "v"), "options: put with sync_on_write");
    expect(opened->remove("k"), "options: remove with sync_on_write");
    expect(opened->put("k2", "v2"), "options: put again");

    Store moved = std::move(*opened);
    expect(moved.get("k2") == "v2", "move: moved-to store works");
    expect(moved.put("k3", "v3"), "move: moved-to store accepts writes");
}

void test_open_rejects_foreign_files() {
    {
        TempFile f("garbage");
        std::ofstream(f.path, std::ios::binary) << "this is definitely not an mmapKV file, just some text";
        expect(!Store::open(f.path).has_value(), "foreign: wrong magic is rejected");
    }
    {
        TempFile f("short");
        std::ofstream(f.path, std::ios::binary) << "abc";
        expect(!Store::open(f.path).has_value(), "foreign: file shorter than the header is rejected");
    }
    {
        TempFile f("empty");
        std::ofstream(f.path, std::ios::binary).flush();
        auto store = Store::open(f.path);
        expect(store.has_value(), "foreign: an empty file becomes a new store");
        expect(store && store->put("k", "v") && store->get("k") == "v", "foreign: and is usable");
    }
    {
        TempFile f("version");
        {
            auto store = Store::open(f.path);
            if (!store) { expect(false, "version: open"); return; }
            expect(store->put("k", "v"), "version: put");
        }
        // The format version is a little-endian u32 at offset 8 of the file header.
        std::fstream file(f.path, std::ios::in | std::ios::out | std::ios::binary);
        const std::uint32_t future = 99;
        file.seekp(8);
        file.write(reinterpret_cast<const char*>(&future), sizeof(future));
        file.close();
        expect(!Store::open(f.path).has_value(), "foreign: unsupported version is rejected");
    }
}

}  // namespace

int main() {
    test_basic_put_get();
    test_remove();
    test_invalid_input();
    test_keys();
    test_persistence();
    test_growth();
    test_large_value();
    test_options_and_move();
    test_open_rejects_foreign_files();

    return failures == 0 ? 0 : 1;
}
