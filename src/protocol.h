#pragma once
#include <iostream>
#include <string>

using namespace std;

enum class CommandType {
    PUT,
    GET,
    AGG,
    STATS,
    FLUSH,
    QUIT,
    UNKNOWN
};

struct Command {
    CommandType type = CommandType::UNKNOWN;
    string error_msg;

    string metric_name;
    int64_t     timestamp = 0;
    double      value     = 0.0;

    int64_t     from_ts   = 0;
    int64_t     to_ts     = 0;

    int64_t     bucket_seconds = 0;
    string agg_func;
};

Command parse_command(const string& line);
