#pragma once
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <atomic>

#include "chunk.h"
#include "wal.h"

using namespace std;

static constexpr size_t DEFAULT_HEAD_CAPACITY = 10000;

struct HeadBlock
{

    vector<int64_t> timestamps;
    vector<double> values;
    int64_t last_timestamp = INT64_MIN;
    size_t capacity = DEFAULT_HEAD_CAPACITY;
    mutable mutex lock;

    enum class AppendResult
    {
        OK,
        OUT_OF_ORDER,
        BLOCK_FULL
    };

    AppendResult append(int64_t ts, double val);

    struct RangeResult
    {
        vector<int64_t> timestamps;
        vector<double> values;
    };

    RangeResult range(int64_t from_ts, int64_t to_ts) const;

    size_t count() const
    {
        return timestamps.size();
    }
    int64_t first_timestamp() const
    {
        return timestamps.empty() ? 0 : timestamps.front();
    }
    int64_t last_ts() const
    {
        return timestamps.empty() ? 0 : timestamps.back();
    }

    // Clear the head block after a flush
    void clear()
    {
        timestamps.clear();
        values.clear();
        last_timestamp = INT64_MIN;
    }
};

// Per-metric disk state: cached chunk metadata sorted by first_ts
struct MetricDiskState
{
    mutable mutex lock;
    vector<ChunkMeta> chunks;       // sorted by first_ts ascending
    vector<ChunkMeta> downsampled;  // downsampled chunks (1pt/min), same sort
    size_t total_disk_points = 0;

    void add_chunk(const ChunkMeta &meta);
    void sort_chunks();
};

// AGG result types
struct AggBucket
{
    int64_t bucket_start = 0;
    int64_t bucket_end = 0;
    double value = 0.0;
    size_t count = 0;
};

struct AggResult
{
    vector<AggBucket> buckets;
};

class MetricRegistry
{
public:
    HeadBlock *get_or_create(const string &name);
    HeadBlock *get(const string &name);
    vector<string> metric_names() const;

    MetricDiskState *get_or_create_disk(const string &name);
    MetricDiskState *get_disk(const string &name);

    FlushStats flush_metric(const string &name, const string &data_dir);

    void scan_data_dir(const string &data_dir);

    HeadBlock::RangeResult full_range(const string &name,
                                      int64_t from_ts, int64_t to_ts);

    AggResult agg_range(const string &name,
                        int64_t from_ts, int64_t to_ts,
                        int64_t bucket_seconds,
                        const string &func);

    // ── Bonus 1: WAL ──────────────────────────────────────────────────────
    WAL *get_or_create_wal(const string &name, const string &data_dir);
    void replay_wals(const string &data_dir);

    // ── Bonus 2: Retention ────────────────────────────────────────────────
    void set_retention(const string &name, int64_t max_age_seconds);
    void set_default_retention(int64_t max_age_seconds);
    void enforce_retention(int64_t now_ts, const string &data_dir);

    // ── Bonus 3: Downsampling ─────────────────────────────────────────────
    void downsample_chunk(const string &name, const string &data_dir,
                          const ChunkMeta &meta, int64_t now_ts);
    void downsample_old_chunks(const string &data_dir, int64_t now_ts);

private:
    mutable mutex map_lock_;
    unordered_map<string, unique_ptr<HeadBlock>> metrics_;
    unordered_map<string, unique_ptr<MetricDiskState>> disk_state_;
    unordered_map<string, unique_ptr<WAL>> wals_;

    // Retention: per-metric max age in seconds (0 = no retention)
    unordered_map<string, int64_t> retention_;
    int64_t default_retention_ = 0;
};
