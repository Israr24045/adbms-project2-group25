#include "storage.h"

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