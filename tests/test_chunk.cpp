// tests/test_chunk.cpp — end-to-end chunk round-trip test
//
// Writes a set of (timestamp, value) pairs to a compressed chunk file,
// reads them back, and verifies exact (bit-for-bit) recovery.

#include "chunk.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

using namespace std;

// Remove a directory and its contents
static void rmdir_recursive(const string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        string name = ent->d_name;
        if (name == "." || name == "..") continue;
        string full = path + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            rmdir_recursive(full);
        else
            remove(full.c_str());
    }
    closedir(d);
    rmdir(path.c_str());
}

// ── Test 1: Small chunk (4 points from Section 2 example) ──────────────────
static void test_small_chunk() {
    cout << "test_small_chunk ... ";

    string dir = "./test_chunk_data/cpu.usage";
    rmdir_recursive("./test_chunk_data");

    vector<int64_t> ts  = {1728000000, 1728000010, 1728000020, 1728000030};
    vector<double>  val = {45.2, 45.8, 46.1, 47.0};

    FlushStats stats = write_chunk(dir, ts, val);
    assert(stats.total_bytes > 0);
    assert(stats.point_count == 4);
    assert(stats.header_bytes == 36);

    // Read it back
    string chunk_path = dir + "/1728000000.chunk";
    ChunkData data = read_chunk(chunk_path);
    assert(data.valid);
    assert(data.timestamps.size() == 4);
    assert(data.values.size() == 4);

    for (size_t i = 0; i < ts.size(); ++i) {
        assert(data.timestamps[i] == ts[i]);
        // Bit-exact comparison via memcpy to uint64
        uint64_t a, b;
        memcpy(&a, &val[i], 8);
        memcpy(&b, &data.values[i], 8);
        assert(a == b);
    }

    assert(data.first_ts == 1728000000);
    assert(data.last_ts  == 1728000030);

    rmdir_recursive("./test_chunk_data");
    cout << "PASS\n";
}

// ── Test 2: Large chunk (1000 regular-interval points, random-walk values) ─
static void test_large_chunk() {
    cout << "test_large_chunk ... ";

    string dir = "./test_chunk_data/sensor.temp";
    rmdir_recursive("./test_chunk_data");

    const int N = 1000;
    vector<int64_t> ts(N);
    vector<double>  val(N);

    srand(42);
    int64_t t = 1700000000;
    double v = 20.0;
    for (int i = 0; i < N; ++i) {
        ts[i] = t;
        val[i] = v;
        t += 10;  // regular 10-second intervals
        v += ((double)(rand() % 100) - 50.0) / 100.0;  // small drift
    }

    FlushStats stats = write_chunk(dir, ts, val);
    assert(stats.total_bytes > 0);
    assert(stats.point_count == (size_t)N);

    // Check compression: should be much smaller than naive (16 bytes * N)
    size_t naive = N * 16;
    cout << "[" << stats.total_bytes << " bytes vs " << naive << " naive, "
         << "ratio " << (double)naive / stats.total_bytes << "x] ";

    // Read back
    string chunk_path = dir + "/" + to_string(ts[0]) + ".chunk";
    ChunkData data = read_chunk(chunk_path);
    assert(data.valid);
    assert(data.timestamps.size() == (size_t)N);
    assert(data.values.size() == (size_t)N);

    for (int i = 0; i < N; ++i) {
        assert(data.timestamps[i] == ts[i]);
        uint64_t a, b;
        memcpy(&a, &val[i], 8);
        memcpy(&b, &data.values[i], 8);
        assert(a == b);
    }

    rmdir_recursive("./test_chunk_data");
    cout << "PASS\n";
}

// ── Test 3: Chunk metadata reader ──────────────────────────────────────────
static void test_chunk_meta() {
    cout << "test_chunk_meta ... ";

    string dir = "./test_chunk_data/mem.free";
    rmdir_recursive("./test_chunk_data");

    vector<int64_t> ts  = {1000, 1010, 1020, 1030, 1040};
    vector<double>  val = {100.0, 200.0, 300.0, 400.0, 500.0};

    write_chunk(dir, ts, val);

    ChunkMeta meta = read_chunk_meta(dir + "/1000.chunk");
    assert(meta.point_count == 5);
    assert(meta.first_ts == 1000);
    assert(meta.last_ts  == 1040);

    rmdir_recursive("./test_chunk_data");
    cout << "PASS\n";
}

// ── Test 4: CRC corruption detection ──────────────────────────────────────
static void test_crc_corruption() {
    cout << "test_crc_corruption ... ";

    string dir = "./test_chunk_data/net.rx";
    rmdir_recursive("./test_chunk_data");

    vector<int64_t> ts  = {100, 200, 300};
    vector<double>  val = {1.0, 2.0, 3.0};

    write_chunk(dir, ts, val);
    string path = dir + "/100.chunk";

    // Corrupt a byte in the middle of the file
    FILE* f = fopen(path.c_str(), "r+b");
    assert(f);
    fseek(f, 20, SEEK_SET);
    uint8_t byte = 0xFF;
    fwrite(&byte, 1, 1, f);
    fclose(f);

    // Reading should fail due to CRC mismatch
    ChunkData data = read_chunk(path);
    assert(!data.valid);

    rmdir_recursive("./test_chunk_data");
    cout << "PASS\n";
}

// ── Test 5: Multiple chunks for the same metric ───────────────────────────
static void test_multiple_chunks() {
    cout << "test_multiple_chunks ... ";

    string dir = "./test_chunk_data/disk.io";
    rmdir_recursive("./test_chunk_data");

    // Write two separate chunks
    vector<int64_t> ts1  = {1000, 1010, 1020};
    vector<double>  val1 = {10.0, 20.0, 30.0};
    write_chunk(dir, ts1, val1);

    vector<int64_t> ts2  = {2000, 2010, 2020};
    vector<double>  val2 = {40.0, 50.0, 60.0};
    write_chunk(dir, ts2, val2);

    // Both should be readable
    ChunkData d1 = read_chunk(dir + "/1000.chunk");
    ChunkData d2 = read_chunk(dir + "/2000.chunk");
    assert(d1.valid && d1.timestamps.size() == 3);
    assert(d2.valid && d2.timestamps.size() == 3);

    assert(d1.timestamps[0] == 1000);
    assert(d2.timestamps[0] == 2000);

    rmdir_recursive("./test_chunk_data");
    cout << "PASS\n";
}

int main() {
    cout << "=== Chunk round-trip tests ===\n";
    test_small_chunk();
    test_large_chunk();
    test_chunk_meta();
    test_crc_corruption();
    test_multiple_chunks();
    cout << "All chunk tests PASS\n";
    return 0;
}
