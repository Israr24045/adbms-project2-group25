#pragma once
#include<iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

#include "chunk.h"

using namespace std;

static constexpr size_t DEFAULT_HEAD_CAPACITY = 4096;

struct HeadBlock {

    vector<int64_t> timestamps;
    vector<double>  values;
    int64_t last_timestamp = INT64_MIN;
    size_t capacity = DEFAULT_HEAD_CAPACITY;
    mutable mutex lock;

    enum class AppendResult { 
        OK,
        OUT_OF_ORDER,
        BLOCK_FULL
    };

    AppendResult append(int64_t ts, double val);

    struct RangeResult {
        vector<int64_t> timestamps;
        vector<double>  values;
    };

    RangeResult range(int64_t from_ts, int64_t to_ts) const;

    size_t  count() const { 
        return timestamps.size(); 
    }
    int64_t first_timestamp() const {
         return timestamps.empty() ? 0 : timestamps.front(); 
    }
    int64_t last_ts() const {
         return timestamps.empty() ? 0 : timestamps.back(); 
    }

    // Clear the head block after a flush
    void clear() {
        timestamps.clear();
        values.clear();
        last_timestamp = INT64_MIN;
    }
};

// Per-metric disk state: cached chunk metadata sorted by first_ts
struct MetricDiskState {
    mutable mutex lock;
    vector<ChunkMeta> chunks;   // sorted by first_ts ascending
    size_t total_disk_points = 0;

    void add_chunk(const ChunkMeta& meta);
    void sort_chunks();
};

class MetricRegistry {
public:
    HeadBlock* get_or_create(const string& name);
    HeadBlock* get(const string& name);
    vector<string> metric_names() const;

    MetricDiskState* get_or_create_disk(const string& name);
    MetricDiskState* get_disk(const string& name);

    FlushStats flush_metric(const string& name, const string& data_dir);

    void scan_data_dir(const string& data_dir);

    HeadBlock::RangeResult full_range(const string& name,
                                     int64_t from_ts, int64_t to_ts);

private:
    mutable mutex map_lock_;
    unordered_map<string, unique_ptr<HeadBlock>>      metrics_;
    unordered_map<string, unique_ptr<MetricDiskState>> disk_state_;
};
