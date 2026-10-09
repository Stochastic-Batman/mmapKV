# mmapKV

[![CI](https://github.com/Stochastic-Batman/mmapKV/actions/workflows/ci.yml/badge.svg)](https://github.com/Stochastic-Batman/mmapKV/actions/workflows/ci.yml)

An append-only key-value store backed by a `mmap`, with CRC-checked records and crash recovery on reopen.

The data file is an append-only log, reads are served straight from the mapped pages, and recovery after a crash is a single linear scan. The goal is a store small enough to read end to end that still handles partial writes correctly. Written in C++20.

## Building

You need `clang++` with C++20 support and GNU `make`. It was developed on Linux.

```
make
```

This builds the command-line tool as `build/mmapkv`, along with the test programs.

## Usage

```
$ ./build/mmapkv data.kv put language "C++20"
$ ./build/mmapkv data.kv put editor nvim
$ ./build/mmapkv data.kv get language
C++20
$ ./build/mmapkv data.kv list
editor
language
$ ./build/mmapkv data.kv del editor
$ ./build/mmapkv data.kv get editor
mmapkv: key not found
```

The data file is created by the first `put`. `get` prints the value followed by a newline, and `list` prints all keys in sorted order. The exit status is 0 on success, 1 if the key was not found (for `get` and `del`), and 2 for any other error. With `--sync` before the file name, every write is flushed to disk before the command returns (see Durability below).

## How it works

1. The data file is mapped into the process's address space, so reading a value is a memory access rather than a `read` call. The operating system loads pages on demand and writes modified pages back to disk.
2. Every write adds a new record to the end of the file. Existing bytes are never modified. For any key, the most recent record wins, and a delete is recorded as a special marker (a tombstone).
3. Each record carries a CRC that covers its header and contents, so a record that was only partly written can be told apart from a valid one.
4. When the store is opened, the log is scanned once to build a hash index from each live key to the location of its latest value. Lookups consult the index and then read the value from the mapping.
5. If the process or machine dies mid-write, the file ends with an incomplete record. On the next open, the scan stops at the first record that is truncated or fails its checksum, and everything after that point is discarded.
6. Data reaches disk through the kernel's normal writeback unless the caller asks for more. How often to force a flush is a trade-off between throughput and how much recent data could be lost on power failure, and it is left to the user.

## Durability and crashes

A write goes into the memory-mapped file, and from there the kernel writes it to disk on its own schedule. If your process crashes or is killed, nothing is lost, because the kernel already owns the modified pages. Data can be lost only if the whole machine loses power or the kernel crashes before those pages reach disk, and then only the most recent writes.

The kernel may write pages out in any order, so a crash can leave a later record on disk while an earlier one is missing. Every record carries a CRC-32C checksum, and opening the store stops at the first record that is truncated or fails its check, so a damaged record and everything after it is discarded. Before the first write after opening, the space after the last good record is zeroed, so a discarded record can never reappear later. Opening and reading never modify the file. A delete that was cut off before it became durable did not happen, so the key comes back with its previous value.

To force data to disk, `sync()` flushes the mapping with `msync` and then calls `fsync`, so that changes to the file size reach disk as well. With the `sync_on_write` option (`--sync` on the command line), every `put` and `remove` is flushed before it returns. This is the safest mode and also much slower. If the flush itself fails, `put` or `remove` returns false even though the record is already in the file, so in this mode false means "not confirmed durable" rather than "not written".

## Testing

```
make test
```

This builds and runs the test programs. They stay silent when everything passes and print the failing checks otherwise. They cover a known checksum value, growing and remapping the file, encoding and decoding of records (including every single-bit corruption and every truncation point), the store across reopens and many remaps, and crash recovery (corrupted records, cut-off files, garbage at the end of the log, and lost deletes).

```
make sanitize
```

This builds everything again in `build-asan/` with clang's AddressSanitizer and UndefinedBehaviorSanitizer, and runs the tests with them. AddressSanitizer instruments every memory access and stops with a report on out-of-bounds reads and writes, use-after-free and leaks, which matters in code that works with raw mapped memory. UndefinedBehaviorSanitizer catches undefined behavior such as misaligned access and signed integer overflow, and it is set to be fatal so that a violation fails the run. Both slow the program down considerably, which is why this is a separate build. It needs the sanitizer runtime that matches your clang version (on Ubuntu 24.04 that is the `libclang-rt-18-dev` package).

Every push and pull request runs both builds on GitHub Actions.

## Limitations

- Overwritten and deleted values are not reclaimed yet; compaction is planned.
- Every live key must fit in memory, since the index is not paged. The whole file is also mapped, so a store cannot be larger than the process's address space.
- One process and one writer at a time. There is no locking, so two processes (or two threads) using the same file can corrupt it.
- The file doubles in size when it fills up, so it can be up to twice as large as its contents. On filesystems with sparse file support the unused tail takes no disk space.
- Keys must not be empty, and keys and values must each be smaller than 4 GiB.
- Built and tested on Linux only. It relies on POSIX calls (`mmap`, `msync`, `fsync` and `ftruncate`), so other POSIX systems may work, but that is untested.
- A damaged record in the middle of the file (for example from disk corruption) is treated as the end of the log. Everything after it is lost to the store, and the first write after that erases it. Until then the data is still on disk.

## License

GPL-3.0. See [LICENSE](LICENSE).
