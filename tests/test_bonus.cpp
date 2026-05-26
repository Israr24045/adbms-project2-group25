#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "storage.h"
#include "wal.h"
#include "chunk.h"

using namespace std;

void cleanup_dir(const string& dir) {
    string cmd = "rm -rf " + dir;
    int rc = system(cmd.c_str());
    (void)rc;
}

void test_wal_basic() {
    cout << "Testing WAL basic functionality..." << endl;
    string test_dir = "./test_wal_dir";
    cleanup_dir(test_dir);
    mkdir(test_dir.c_str(), 0755);

    string wal_path = test_dir + "/test.wal";
    {
        WAL wal(wal_path);
        assert(wal.append(1000, 1.5));
        assert(wal.append(2000, 2.5));
        assert(wal.append(3000, 3.5));
    } // WAL closed here

    auto recovered = WAL::replay(wal_path);
    assert(recovered.size() == 3);
    assert(recovered[0].first == 1000 && recovered[0].second == 1.5);
    assert(recovered[1].first == 2000 && recovered[1].second == 2.5);
    assert(recovered[2].first == 3000 && recovered[2].second == 3.5);

    // Test truncate
    {
        WAL wal(wal_path);
        wal.truncate();
    }
    recovered = WAL::replay(wal_path);
    assert(recovered.empty());

    cleanup_dir(test_dir);
    cout << "  -> test_wal_basic: PASS" << endl;
}

void test_wal_replay_recovery() {
    cout << "Testing WAL recovery integration in registry..." << endl;
    string test_dir = "./test_recovery_dir";
    cleanup_dir(test_dir);
    mkdir(test_dir.c_str(), 0755);

    // 1. Create a metric registry, write to it, which writes to WAL
    {
        MetricRegistry registry;
        WAL* wal = registry.get_or_create_wal("cpu.usage", test_dir);
        HeadBlock* block = registry.get_or_create("cpu.usage");
        
        block->append(100, 10.5);
        wal->append(100, 10.5);

        block->append(200, 12.0);
        wal->append(200, 12.0);
    } // Both closed

    // 2. Load a fresh registry and replay the WAL
    {
        MetricRegistry registry;
        registry.replay_wals(test_dir);

        // Verify cpu.usage has the recovered points in memory
        HeadBlock* block = registry.get("cpu.usage");
        assert(block != nullptr);
        assert(block->count() == 2);
        assert(block->timestamps[0] == 100);
        assert(block->values[0] == 10.5);
        assert(block->timestamps[1] == 200);
        assert(block->values[1] == 12.0);
    }

    cleanup_dir(test_dir);
    cout << "  -> test_wal_replay_recovery: PASS" << endl;
}

void test_retention() {
    cout << "Testing Retention Policy enforcement..." << endl;
    string test_dir = "./test_retention_dir";
    cleanup_dir(test_dir);
    mkdir(test_dir.c_str(), 0755);

    MetricRegistry registry;
    registry.set_default_retention(10); // 10 seconds retention

    // Let's create some dummy points and flush them to create a chunk file
    string metric_name = "temp.sensor";
    HeadBlock* block = registry.get_or_create(metric_name);
    block->append(1000, 22.5);
    block->append(1001, 23.0);
    
    // Flush to create a chunk
    registry.flush_metric(metric_name, test_dir);

    MetricDiskState* ds = registry.get_disk(metric_name);
    assert(ds != nullptr);
    assert(ds->chunks.size() == 1);
    string chunk_path = ds->chunks[0].filepath;
    
    // Ensure file exists on disk
    struct stat st;
    assert(stat(chunk_path.c_str(), &st) == 0);

    // Enforce retention with now_ts = 1005 (chunk last_ts is 1001, age is 4s, should survive)
    registry.enforce_retention(1005, test_dir);
    assert(ds->chunks.size() == 1);
    assert(stat(chunk_path.c_str(), &st) == 0);

    // Enforce retention with now_ts = 1015 (chunk last_ts is 1001, age is 14s > 10s retention, should be purged)
    registry.enforce_retention(1015, test_dir);
    assert(ds->chunks.size() == 0);
    assert(stat(chunk_path.c_str(), &st) != 0); // File deleted

    cleanup_dir(test_dir);
    cout << "  -> test_retention: PASS" << endl;
}

void test_downsampling() {
    cout << "Testing Downsampling of old chunks..." << endl;
    string test_dir = "./test_downsample_dir";
    cleanup_dir(test_dir);
    mkdir(test_dir.c_str(), 0755);

    MetricRegistry registry;
    string metric_name = "network.bytes";
    HeadBlock* block = registry.get_or_create(metric_name);

    // Let's insert 120 points, one point per second, starting at t=0
    for (int i = 0; i < 120; ++i) {
        block->append(i, (double)i);
    }

    // Flush to create a raw chunk
    registry.flush_metric(metric_name, test_dir);
    
    MetricDiskState* ds = registry.get_disk(metric_name);
    assert(ds != nullptr);
    assert(ds->chunks.size() == 1);

    // Now, run downsampling on old chunks.
    // The chunk ends at t=119. Let's say now_ts = 5000 (which is > 3600 seconds, so the chunk is older than 1 hour)
    registry.downsample_old_chunks(test_dir, 5000);

    // Check that we have a downsampled chunk registered
    assert(ds->downsampled.size() == 1);
    assert(ds->downsampled[0].point_count == 2); // 120 seconds = 2 minutes = 2 downsampled points

    // Decompress and verify the downsampled values
    ChunkData ds_chunk = read_chunk(ds->downsampled[0].filepath);
    assert(ds_chunk.valid);
    assert(ds_chunk.timestamps.size() == 2);
    // Timestamps should be aligned to minute boundaries (0 and 60)
    assert(ds_chunk.timestamps[0] == 0);
    assert(ds_chunk.timestamps[1] == 60);

    // Average of 0..59 is 29.5
    // Average of 60..119 is 89.5
    assert(abs(ds_chunk.values[0] - 29.5) < 1e-6);
    assert(abs(ds_chunk.values[1] - 89.5) < 1e-6);

    // Test querying AGG avg with bucket = 60s uses the downsampled chunk!
    auto agg = registry.agg_range(metric_name, 0, 120, 60, "avg");
    assert(agg.buckets.size() == 2);
    assert(agg.buckets[0].bucket_start == 0);
    assert(agg.buckets[0].value == 29.5);
    assert(agg.buckets[1].bucket_start == 60);
    assert(agg.buckets[1].value == 89.5);

    cleanup_dir(test_dir);
    cout << "  -> test_downsampling: PASS" << endl;
}

int main() {
    cout << "=== Running Part 3 Integration Tests ===" << endl;
    test_wal_basic();
    test_wal_replay_recovery();
    test_retention();
    test_downsampling();
    cout << "All Part 3 tests passed successfully!" << endl;
    return 0;
}
