#include "storage.h"

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <iostream>
#include <limits>
#include <cstdio>
#include <chrono>

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
// MetricRegistry — basic operations
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

    // Truncate WAL after successful flush
    {
        lock_guard<mutex> lk(map_lock_);
        auto it = wals_.find(name);
        if (it != wals_.end())
        {
            it->second->truncate();
        }
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

        // Scan downsampled chunks
        string ds_dir = metric_path + "/downsampled";
        DIR *dsdir = opendir(ds_dir.c_str());
        if (dsdir)
        {
            while ((chunk_entry = readdir(dsdir)) != nullptr)
            {
                string fname = chunk_entry->d_name;
                if (fname.size() < 7 || fname.substr(fname.size() - 6) != ".chunk")
                    continue;
                string chunk_path = ds_dir + "/" + fname;
                ChunkMeta meta = read_chunk_meta(chunk_path);
                if (meta.point_count > 0)
                {
                    lock_guard<mutex> lk(ds->lock);
                    ds->downsampled.push_back(meta);
                }
            }
            closedir(dsdir);

            lock_guard<mutex> lk(ds->lock);
            sort(ds->downsampled.begin(), ds->downsampled.end(),
                 [](const ChunkMeta &a, const ChunkMeta &b) { return a.first_ts < b.first_ts; });
        }

        // Sort chunks by first_ts
        {
            lock_guard<mutex> lk(ds->lock);
            ds->sort_chunks();
        }

        cout << "  loaded metric: " << metric_name
             << " (" << ds->chunks.size() << " chunks, "
             << ds->total_disk_points << " points on disk";
        if (!ds->downsampled.empty())
            cout << ", " << ds->downsampled.size() << " downsampled";
        cout << ")\n";
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

// ─────────────────────────────────────────────────────────────────────────────
// AGG query — uses downsampled data when available
// ─────────────────────────────────────────────────────────────────────────────

AggResult MetricRegistry::agg_range(const string &name,
                                    int64_t from_ts, int64_t to_ts,
                                    int64_t bucket_seconds,
                                    const string &func)
{
    AggResult result;

    // For large bucket sizes (>= 60s), try to use downsampled data
    bool use_downsampled = (bucket_seconds >= 60);
    HeadBlock::RangeResult raw;

    if (use_downsampled)
    {
        MetricDiskState *ds = get_disk(name);
        if (ds)
        {
            // Collect downsampled chunks that cover the range
            vector<ChunkMeta> ds_chunks;
            {
                lock_guard<mutex> lk(ds->lock);
                ds_chunks = ds->downsampled;
            }

            bool has_coverage = false;
            for (const auto &meta : ds_chunks)
            {
                if (meta.last_ts >= from_ts && meta.first_ts < to_ts)
                {
                    has_coverage = true;
                    ChunkData chunk = read_chunk(meta.filepath);
                    if (!chunk.valid)
                        continue;
                    for (size_t i = 0; i < chunk.timestamps.size(); ++i)
                    {
                        if (chunk.timestamps[i] >= from_ts && chunk.timestamps[i] < to_ts)
                        {
                            raw.timestamps.push_back(chunk.timestamps[i]);
                            raw.values.push_back(chunk.values[i]);
                        }
                    }
                }
            }

            if (!has_coverage)
            {
                // Fall back to raw data
                raw = full_range(name, from_ts, to_ts);
            }
            else
            {
                // Also add head block data (not downsampled)
                HeadBlock *block = get(name);
                if (block)
                {
                    lock_guard<mutex> lk(block->lock);
                    auto head = block->range(from_ts, to_ts);
                    raw.timestamps.insert(raw.timestamps.end(),
                                          head.timestamps.begin(), head.timestamps.end());
                    raw.values.insert(raw.values.end(),
                                      head.values.begin(), head.values.end());
                }
            }
        }
        else
        {
            raw = full_range(name, from_ts, to_ts);
        }
    }
    else
    {
        raw = full_range(name, from_ts, to_ts);
    }

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

// ─────────────────────────────────────────────────────────────────────────────
// Bonus 1: WAL
// ─────────────────────────────────────────────────────────────────────────────

WAL *MetricRegistry::get_or_create_wal(const string &name, const string &data_dir)
{
    lock_guard<mutex> lk(map_lock_);

    auto it = wals_.find(name);
    if (it != wals_.end())
        return it->second.get();

    // Ensure metric directory exists
    string metric_dir = data_dir + "/" + name;
    struct stat st;
    if (stat(metric_dir.c_str(), &st) != 0)
        mkdir(metric_dir.c_str(), 0755);

    string wal_path = metric_dir + "/wal.log";
    auto wal = make_unique<WAL>(wal_path);
    WAL *raw = wal.get();
    wals_.emplace(name, move(wal));
    return raw;
}

void MetricRegistry::replay_wals(const string &data_dir)
{
    DIR *top = opendir(data_dir.c_str());
    if (!top)
        return;

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

        string wal_path = metric_path + "/wal.log";
        if (stat(wal_path.c_str(), &st) != 0 || st.st_size < 16)
            continue;

        string metric_name = dname;
        auto entries = WAL::replay(wal_path);
        if (entries.empty())
            continue;

        // Replay into head block
        HeadBlock *block = get_or_create(metric_name);
        size_t recovered = 0;
        {
            lock_guard<mutex> lk(block->lock);
            for (const auto &[ts, val] : entries)
            {
                auto r = block->append(ts, val);
                if (r == HeadBlock::AppendResult::OK)
                    ++recovered;
            }
        }

        if (recovered > 0)
        {
            cout << "  WAL recovery: " << metric_name
                 << " — " << recovered << " points recovered\n";
        }

        // Create the WAL object (it will open in append mode)
        get_or_create_wal(metric_name, data_dir);
    }
    closedir(top);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bonus 2: Retention Policies
// ─────────────────────────────────────────────────────────────────────────────

void MetricRegistry::set_retention(const string &name, int64_t max_age_seconds)
{
    lock_guard<mutex> lk(map_lock_);
    retention_[name] = max_age_seconds;
}

void MetricRegistry::set_default_retention(int64_t max_age_seconds)
{
    lock_guard<mutex> lk(map_lock_);
    default_retention_ = max_age_seconds;
}

void MetricRegistry::enforce_retention(int64_t now_ts, const string &data_dir)
{
    // Collect metrics and their retention policies
    vector<pair<string, int64_t>> policies;
    {
        lock_guard<mutex> lk(map_lock_);
        for (const auto &[name, _] : disk_state_)
        {
            auto it = retention_.find(name);
            int64_t max_age = (it != retention_.end()) ? it->second : default_retention_;
            if (max_age > 0)
                policies.emplace_back(name, max_age);
        }
    }

    for (const auto &[metric, max_age] : policies)
    {
        int64_t cutoff = now_ts - max_age;

        MetricDiskState *ds = get_disk(metric);
        if (!ds)
            continue;

        // Find expired chunks under lock, remove from vector
        vector<string> to_delete;
        {
            lock_guard<mutex> lk(ds->lock);
            auto it = ds->chunks.begin();
            while (it != ds->chunks.end())
            {
                if (it->last_ts < cutoff)
                {
                    to_delete.push_back(it->filepath);
                    ds->total_disk_points -= it->point_count;
                    it = ds->chunks.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            // Also delete expired downsampled chunks
            auto dit = ds->downsampled.begin();
            while (dit != ds->downsampled.end())
            {
                if (dit->last_ts < cutoff)
                {
                    to_delete.push_back(dit->filepath);
                    dit = ds->downsampled.erase(dit);
                }
                else
                {
                    ++dit;
                }
            }
        }

        // Delete files outside the lock (safe: chunk is already removed from
        // the index, so no new query will try to read it; if an in-flight
        // query already copied the path, read_chunk returns valid=false)
        for (const auto &path : to_delete)
        {
            if (remove(path.c_str()) == 0)
            {
                cerr << "retention: deleted " << path << "\n";
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Bonus 3: Downsampling
// ─────────────────────────────────────────────────────────────────────────────

void MetricRegistry::downsample_chunk(const string &name, const string &data_dir,
                                       const ChunkMeta &meta, int64_t /*now_ts*/)
{
    // Read the raw chunk
    ChunkData chunk = read_chunk(meta.filepath);
    if (!chunk.valid || chunk.timestamps.empty())
        return;

    // Group into 60-second buckets and average
    vector<int64_t> ds_ts;
    vector<double> ds_vals;

    size_t i = 0;
    while (i < chunk.timestamps.size())
    {
        int64_t bucket_start = (chunk.timestamps[i] / 60) * 60;
        int64_t bucket_end = bucket_start + 60;
        double sum = 0.0;
        size_t count = 0;

        while (i < chunk.timestamps.size() && chunk.timestamps[i] < bucket_end)
        {
            sum += chunk.values[i];
            ++count;
            ++i;
        }

        ds_ts.push_back(bucket_start);
        ds_vals.push_back(sum / (double)count);
    }

    if (ds_ts.empty())
        return;

    // Write to downsampled directory
    string ds_dir = data_dir + "/" + name + "/downsampled";
    write_chunk(ds_dir, ds_ts, ds_vals);

    // Register in disk state
    ChunkMeta ds_meta;
    ds_meta.filepath = ds_dir + "/" + to_string(ds_ts.front()) + ".chunk";
    ds_meta.first_ts = ds_ts.front();
    ds_meta.last_ts = ds_ts.back();
    ds_meta.point_count = (uint32_t)ds_ts.size();

    MetricDiskState *ds = get_or_create_disk(name);
    lock_guard<mutex> lk(ds->lock);
    ds->downsampled.push_back(ds_meta);
    sort(ds->downsampled.begin(), ds->downsampled.end(),
         [](const ChunkMeta &a, const ChunkMeta &b) { return a.first_ts < b.first_ts; });
}

void MetricRegistry::downsample_old_chunks(const string &data_dir, int64_t now_ts)
{
    auto names = metric_names();
    int64_t one_hour = 3600;

    for (const auto &name : names)
    {
        MetricDiskState *ds = get_disk(name);
        if (!ds)
            continue;

        // Collect chunks that are old enough and not yet downsampled
        vector<ChunkMeta> to_downsample;
        {
            lock_guard<mutex> lk(ds->lock);
            for (const auto &meta : ds->chunks)
            {
                // Chunk is older than 1 hour
                if (meta.last_ts < now_ts - one_hour)
                {
                    // Check if already downsampled
                    bool already = false;
                    for (const auto &dmeta : ds->downsampled)
                    {
                        if (dmeta.first_ts == (meta.first_ts / 60) * 60)
                        {
                            already = true;
                            break;
                        }
                    }
                    if (!already)
                        to_downsample.push_back(meta);
                }
            }
        }

        for (const auto &meta : to_downsample)
        {
            downsample_chunk(name, data_dir, meta, now_ts);
            cerr << "downsampled: " << meta.filepath << " ("
                 << meta.point_count << " pts → 1pt/min)\n";
        }
    }
}
