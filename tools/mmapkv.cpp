#include <mmapkv/store.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitNotFound = 1;
constexpr int kExitError = 2;

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "usage: mmapkv [--sync] <file> <command> [args]\n"
                 "\n"
                 "commands:\n"
                 "  put <key> <value>   store a value under a key\n"
                 "  get <key>           print the value of a key\n"
                 "  del <key>           delete a key\n"
                 "  list                print all keys, sorted\n"
                 "\n"
                 "options:\n"
                 "  --sync              flush every write to disk before returning\n"
                 "\n"
                 "exit status: 0 success, 1 key not found, 2 error\n");
}

int fail(const char* message) {
    std::fprintf(stderr, "mmapkv: %s\n", message);
    return kExitError;
}

void print_line(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.size() == 1 && (args[0] == "-h" || args[0] == "--help")) {
        print_usage(stdout);
        return kExitOk;
    }

    mmapkv::Options options;
    if (!args.empty() && args[0] == "--sync") {
        options.sync_on_write = true;
        args.erase(args.begin());
    }

    if (args.size() < 2) {
        print_usage(stderr);
        return kExitError;
    }

    const std::string& path = args[0];
    const std::string& command = args[1];
    const std::size_t operands = args.size() - 2;

    const bool valid = (command == "put" && operands == 2) || ((command == "get" || command == "del") && operands == 1) || (command == "list" && operands == 0);

    if (!valid) {
        print_usage(stderr);
        return kExitError;
    }

    // Opening a store creates the file, so only put is allowed to do that.
    if (command != "put" && !std::filesystem::exists(path)) {
        return fail("no such file");
    }

    std::optional<mmapkv::Store> store = mmapkv::Store::open(path, options);
    if (!store) {
        return fail("cannot open the store (the file is not a mmapKV file, or cannot be opened)");
    }

    if (command == "put") {
        if (!store->put(args[2], args[3])) {
            return fail("put failed (empty key, key or value too large, or write error)");
        }
        return kExitOk;
    }

    if (command == "get") {
        const std::optional<std::string> value = store->get(args[2]);
        if (!value) {
            std::fprintf(stderr, "mmapkv: key not found\n");
            return kExitNotFound;
        }
        print_line(*value);
        return kExitOk;
    }

    if (command == "del") {
        const std::size_t before = store->size();
        if (!store->remove(args[2])) {
            return fail("del failed (empty key or write error)");
        }
        if (store->size() == before) {
            std::fprintf(stderr, "mmapkv: key not found\n");
            return kExitNotFound;
        }
        return kExitOk;
    }

    std::vector<std::string> keys = store->keys();
    std::sort(keys.begin(), keys.end());
    for (const std::string& key : keys) {
        print_line(key);
    }

    return kExitOk;
}
