#pragma once
#include<iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

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
};

class MetricRegistry {
public:
    HeadBlock* get_or_create(const string& name);
    HeadBlock* get(const string& name);
    vector<string> metric_names() const;

private:
    mutable mutex map_lock_;
    unordered_map<string, unique_ptr<HeadBlock>> metrics_;
};
