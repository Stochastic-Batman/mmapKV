# mmapKV

An append-only key-value store backed by a `mmap`, with CRC-checked records and crash recovery on reopen.

The data file is an append-only log, reads are served straight from the mapped pages, and recovery after a crash is a single linear scan. The goal is a store small enough to read end to end that still handles partial writes correctly. Written in C++20.

## How it works

1. The data file is mapped into the process's address space, so reading a value is a memory access rather than a `read` call. The operating system loads pages on demand and writes modified pages back to disk.
2. Every write adds a new record to the end of the file. Existing bytes are never modified. For any key, the most recent record wins, and a delete is recorded as a special marker (a tombstone).
3. Each record carries a CRC that covers its header and contents, so a record that was only partly written can be told apart from a valid one.
4. When the store is opened, the log is scanned once to build a hash index from each live key to the location of its latest value. Lookups consult the index and then read the value from the mapping.
5. If the process or machine dies mid-write, the file ends with an incomplete record. On the next open, the scan stops at the first record that is truncated or fails its checksum, and everything after that point is discarded.
6. Data reaches disk through the kernel's normal writeback unless the caller asks for more. How often to force a flush is a trade-off between throughput and how much recent data could be lost on power failure, and it is left to the user.

## Limitations

- Overwritten and deleted values are not reclaimed yet; compaction is planned.
- Every live key must fit in memory, since the index is not paged.
- One process and one writer at a time. There is no locking between processes.
- POSIX only (Linux and macOS, though I built this on Linux), because it relies on `mmap`, `msync`, and `ftruncate`.

## License

GPL-3.0. See [LICENSE](LICENSE).
