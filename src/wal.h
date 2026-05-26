#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <utility>
using namespace std;

// ── Write-Ahead Log (per-metric) ──────────────────────────────────────────
//
// Binary append-only log: each entry is 16 bytes [int64_t ts][double val].
// Uses O_APPEND for atomic writes (no fsync per write — acceptable for TSDB).
// Truncated after each successful flush to disk.

class WAL {
public:
    explicit WAL(const string& path);
    ~WAL();
    // Append a data point to buffer. Returns true on success.
    bool append(int64_t ts, double val);
    // Flush the buffered entries to disk.
    void flush();
    // Truncate (clear) the WAL after a successful flush.
    void truncate();
    // Replay all entries from a WAL file. Returns (timestamp, value) pairs.
    static vector<pair<int64_t, double>> replay(const string& path);
private:
    string path_;
    int fd_ = -1;
    mutex lock_;
    vector<uint8_t> buf_;
    void open_for_append();
};
