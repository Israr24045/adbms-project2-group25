#include "storage.h"

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <iostream>
#include <limits>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// HeadBlock
// ─────────────────────────────────────────────────────────────────────────────

HeadBlock::AppendResult HeadBlock::append(int64_t ts, double val)
{
    // Monotonicity check: reject timestamps strictly less than the last one
    if (ts < last_timestamp)
    {
        return AppendResult::OUT_OF_ORDER;
    }

    // Capacity check: head block is full
    if (timestamps.size() >= capacity)
    {
        return AppendResult::BLOCK_FULL;
    }

    timestamps.push_back(ts);
    values.push_back(val);
    last_timestamp = ts;

    return AppendResult::OK;
}

HeadBlock::RangeResult HeadBlock::range(int64_t from_ts, int64_t to_ts) const
{
    RangeResult result;

    // Half-open range [from_ts, to_ts)
    for (size_t i = 0; i < timestamps.size(); ++i)
    {
        if (timestamps[i] >= from_ts && timestamps[i] < to_ts)
        {
            result.timestamps.push_back(timestamps[i]);
            result.values.push_back(values[i]);
        }
        // Since timestamps are in ascending order, stop early if past the range
        if (timestamps[i] >= to_ts)
        {
            break;
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// MetricDiskState
// ─────────────────────────────────────────────────────────────────────────────

void MetricDiskState::add_chunk(const ChunkMeta &meta)
{
    chunks.push_back(meta);
    total_disk_points += meta.point_count;
}

void MetricDiskState::sort_chunks()
{
    sort(chunks.begin(), chunks.end(),
         [](const ChunkMeta &a, const ChunkMeta &b)
         {
             return a.first_ts < b.first_ts;
         });
}

// ─────────────────────────────────────────────────────────────────────────────
// MetricRegistry
// ─────────────────────────────────────────────────────────────────────────────

HeadBlock *MetricRegistry::get_or_create(const string &name)
{
    lock_guard<mutex> lk(map_lock_);

    auto it = metrics_.find(name);
    if (it != metrics_.end())
    {
        return it->second.get();
    }

    // Create a new head block for this metric
    auto block = make_unique<HeadBlock>();
    HeadBlock *raw = block.get();
    metrics_.emplace(name, move(block));
    return raw;
}

HeadBlock *MetricRegistry::get(const string &name)
{
    lock_guard<mutex> lk(map_lock_);

    auto it = metrics_.find(name);
    if (it != metrics_.end())
    {
        return it->second.get();
    }
    return nullptr;
}

vector<string> MetricRegistry::metric_names() const
{
    lock_guard<mutex> lk(map_lock_);

    vector<string> names;
    names.reserve(metrics_.size());
    for (const auto &pair : metrics_)
    {
        names.push_back(pair.first);
    }
    return names;
}

MetricDiskState *MetricRegistry::get_or_create_disk(const string &name)
{
    lock_guard<mutex> lk(map_lock_);

    auto it = disk_state_.find(name);
    if (it != disk_state_.end())
        return it->second.get();

    auto ds = make_unique<MetricDiskState>();
    MetricDiskState *raw = ds.get();
    disk_state_.emplace(name, move(ds));
    return raw;
}

MetricDiskState *MetricRegistry::get_disk(const string &name)
{
    lock_guard<mutex> lk(map_lock_);

    auto it = disk_state_.find(name);
    if (it != disk_state_.end())
        return it->second.get();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Flush
// ─────────────────────────────────────────────────────────────────────────────

FlushStats MetricRegistry::flush_metric(const string &name, const string &data_dir)
{
    HeadBlock *block = get(name);
    if (!block)
        return {};

    // Take the data out of the head block under lock
    vector<int64_t> ts_copy;
    vector<double> val_copy;
    {
        lock_guard<mutex> lk(block->lock);
        if (block->timestamps.empty())
            return {};
        ts_copy = block->timestamps;
        val_copy = block->values;
        block->clear();
    }

    // Write the chunk (no lock needed — writing to a new file)
    string metric_dir = data_dir + "/" + name;
    FlushStats stats = write_chunk(metric_dir, ts_copy, val_copy);

    if (stats.total_bytes > 0)
    {
        // Register the chunk in disk state
        ChunkMeta meta;
        meta.filepath = metric_dir + "/" + to_string(ts_copy.front()) + ".chunk";
        meta.first_ts = ts_copy.front();
        meta.last_ts = ts_copy.back();
        meta.point_count = (uint32_t)ts_copy.size();

        MetricDiskState *ds = get_or_create_disk(name);
        lock_guard<mutex> lk(ds->lock);
        ds->add_chunk(meta);
        ds->sort_chunks();
    }

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scan data directory at startup
// ─────────────────────────────────────────────────────────────────────────────

void MetricRegistry::scan_data_dir(const string &data_dir)
{
    DIR *top = opendir(data_dir.c_str());
    if (!top)
        return; // No data directory yet — that's OK

    struct dirent *entry;
    while ((entry = readdir(top)) != nullptr)
    {
        string dname = entry->d_name;
        if (dname == "." || dname == "..")
            continue;

        string metric_path = data_dir + "/" + dname;
        struct stat st;
        if (stat(metric_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        // This subdirectory is a metric name
        string metric_name = dname;

        // Ensure the metric exists in registry
        get_or_create(metric_name);
        MetricDiskState *ds = get_or_create_disk(metric_name);

        // Scan chunk files
        DIR *mdir = opendir(metric_path.c_str());
        if (!mdir)
            continue;

        struct dirent *chunk_entry;
        while ((chunk_entry = readdir(mdir)) != nullptr)
        {
            string fname = chunk_entry->d_name;
            if (fname.size() < 7)
                continue; // need at least "X.chunk"
            if (fname.substr(fname.size() - 6) != ".chunk")
                continue;

            string chunk_path = metric_path + "/" + fname;
            ChunkMeta meta = read_chunk_meta(chunk_path);
            if (meta.point_count > 0)
            {
                lock_guard<mutex> lk(ds->lock);
                ds->add_chunk(meta);
            }
        }
        closedir(mdir);

        // Sort chunks by first_ts
        {
            lock_guard<mutex> lk(ds->lock);
            ds->sort_chunks();
        }

        cout << "  loaded metric: " << metric_name
             << " (" << ds->chunks.size() << " chunks, "
             << ds->total_disk_points << " points on disk)\n";
    }
    closedir(top);
}

// ─────────────────────────────────────────────────────────────────────────────
// Full range query: disk chunks + head block
// ─────────────────────────────────────────────────────────────────────────────

HeadBlock::RangeResult MetricRegistry::full_range(const string &name,
                                                  int64_t from_ts, int64_t to_ts)
{
    HeadBlock::RangeResult result;

    // 1. Read matching chunks from disk
    MetricDiskState *ds = get_disk(name);
    if (ds)
    {
        lock_guard<mutex> lk(ds->lock);
        for (const auto &meta : ds->chunks)
        {
            // Skip chunks that don't overlap the query range [from_ts, to_ts)
            if (meta.last_ts < from_ts || meta.first_ts >= to_ts)
                continue;

            // Decompress this chunk
            ChunkData chunk = read_chunk(meta.filepath);
            if (!chunk.valid)
                continue;

            for (size_t i = 0; i < chunk.timestamps.size(); ++i)
            {
                int64_t ts = chunk.timestamps[i];
                if (ts >= from_ts && ts < to_ts)
                {
                    result.timestamps.push_back(ts);
                    result.values.push_back(chunk.values[i]);
                }
                if (ts >= to_ts)
                    break;
            }
        }
    }

    // 2. Read from head block
    HeadBlock *block = get(name);
    if (block)
    {
        lock_guard<mutex> lk(block->lock);
        auto head_range = block->range(from_ts, to_ts);
        result.timestamps.insert(result.timestamps.end(),
                                 head_range.timestamps.begin(),
                                 head_range.timestamps.end());
        result.values.insert(result.values.end(),
                             head_range.values.begin(),
                             head_range.values.end());
    }

    return result;
}

AggResult MetricRegistry::agg_range(const string &name,
                                    int64_t from_ts, int64_t to_ts,
                                    int64_t bucket_seconds,
                                    const string &func)
{
    AggResult result;

    HeadBlock::RangeResult raw = full_range(name, from_ts, to_ts);
    const auto &ts = raw.timestamps;
    const auto &val = raw.values;

    if (ts.empty())
        return result;

    size_t i = 0;
    int64_t bucket_start = from_ts;

    while (bucket_start < to_ts)
    {
        int64_t bucket_end = bucket_start + bucket_seconds;

        double sum_val = 0.0;
        double min_val = numeric_limits<double>::max();
        double max_val = numeric_limits<double>::lowest();
        size_t count = 0;

        while (i < ts.size() && ts[i] < bucket_end)
        {
            double v = val[i];
            sum_val += v;
            if (v < min_val)
                min_val = v;
            if (v > max_val)
                max_val = v;
            ++count;
            ++i;
        }

        if (count > 0)
        {
            AggBucket bucket;
            bucket.bucket_start = bucket_start;
            bucket.bucket_end = bucket_end;
            bucket.count = count;

            if (func == "avg")
                bucket.value = sum_val / (double)count;
            else if (func == "sum")
                bucket.value = sum_val;
            else if (func == "min")
                bucket.value = min_val;
            else if (func == "max")
                bucket.value = max_val;
            else if (func == "count")
                bucket.value = (double)count;

            result.buckets.push_back(bucket);
        }

        bucket_start = bucket_end;
        if (i >= ts.size())
            break;
    }

    return result;
}
