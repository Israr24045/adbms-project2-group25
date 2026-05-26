#include "wal.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <iostream>

using namespace std;

// ── Constructor / Destructor ──────────────────────────────────────────────

WAL::WAL(const string& path) : path_(path) {
    open_for_append();
}

WAL::~WAL() {
    if (fd_ >= 0) ::close(fd_);
}

void WAL::open_for_append() {
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        cerr << "WAL: cannot open " << path_ << ": " << strerror(errno) << "\n";
    }
}

// ── Append ────────────────────────────────────────────────────────────────
// Each entry is exactly 16 bytes: [int64_t timestamp][double value]
// O_APPEND guarantees atomic writes for sizes <= PIPE_BUF (4096 on Linux).

bool WAL::append(int64_t ts, double val) {
    if (fd_ < 0) return false;

    lock_guard<mutex> lk(lock_);
    size_t cur = buf_.size();
    buf_.resize(cur + 16);
    memcpy(buf_.data() + cur, &ts, 8);
    memcpy(buf_.data() + cur + 8, &val, 8);

    if (buf_.size() >= 4096) {
        ssize_t written = ::write(fd_, buf_.data(), buf_.size());
        buf_.clear();
        return written > 0;
    }
    return true;
}

void WAL::flush() {
    lock_guard<mutex> lk(lock_);
    if (!buf_.empty() && fd_ >= 0) {
        ::write(fd_, buf_.data(), buf_.size());
        buf_.clear();
    }
}

// ── Truncate ──────────────────────────────────────────────────────────────
// Called after a successful flush.  Re-opens the file with O_TRUNC to clear.

void WAL::truncate() {
    lock_guard<mutex> lk(lock_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        cerr << "WAL: cannot truncate " << path_ << ": " << strerror(errno) << "\n";
    }
}

// ── Replay ────────────────────────────────────────────────────────────────
// Read all 16-byte entries from the WAL file.  Called at startup.

vector<pair<int64_t, double>> WAL::replay(const string& path) {
    vector<pair<int64_t, double>> entries;

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return entries;  // No WAL file — nothing to replay

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 16) {
        ::close(fd);
        return entries;
    }

    size_t n_entries = (size_t)st.st_size / 16;
    entries.reserve(n_entries);

    uint8_t buf[16];
    for (size_t i = 0; i < n_entries; i++) {
        ssize_t rd = ::read(fd, buf, 16);
        if (rd != 16) break;

        int64_t ts;
        double val;
        memcpy(&ts, buf, 8);
        memcpy(&val, buf + 8, 8);
        entries.emplace_back(ts, val);
    }

    ::close(fd);
    return entries;
}
