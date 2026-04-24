#include "protocol.h"

#include <sstream>
#include <vector>
#include <stdexcept>

using namespace std;

// Split a line into whitespace-separated tokens
static vector<string> tokenize(const string &line)
{
    vector<string> tokens;
    istringstream iss(line);
    string tok;
    while (iss >> tok)
    {
        tokens.push_back(tok);
    }
    return tokens;
}

// Safe string-to-int64 conversion
static bool parse_int64(const string &s, int64_t &out)
{
    try
    {
        size_t pos;
        out = stoll(s, &pos);
        return pos == s.size();
    }
    catch (...)
    {
        return false;
    }
}

// Safe string-to-double conversion
static bool parse_double(const string &s, double &out)
{
    try
    {
        size_t pos;
        out = stod(s, &pos);
        return pos == s.size();
    }
    catch (...)
    {
        return false;
    }
}

// Validate metric name: letters, digits, dots, underscores only
static bool valid_metric_name(const string &name)
{
    if (name.empty())
        return false;
    for (char c : name)
    {
        if (!isalnum(c) && c != '.' && c != '_')
            return false;
    }
    return true;
}

Command parse_command(const string &line)
{
    Command cmd;
    vector<string> tokens = tokenize(line);

    if (tokens.empty())
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "empty command";
        return cmd;
    }

    const string &name = tokens[0];

    // ── PUT metric_name timestamp value ──────────────────────────────────
    if (name == "PUT")
    {
        if (tokens.size() != 4)
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "PUT requires exactly 3 arguments: metric_name timestamp value";
            return cmd;
        }
        if (!valid_metric_name(tokens[1]))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid metric name: " + tokens[1];
            return cmd;
        }
        int64_t ts;
        double val;
        if (!parse_int64(tokens[2], ts))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid timestamp: " + tokens[2];
            return cmd;
        }
        if (!parse_double(tokens[3], val))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid value: " + tokens[3];
            return cmd;
        }
        cmd.type = CommandType::PUT;
        cmd.metric_name = tokens[1];
        cmd.timestamp = ts;
        cmd.value = val;
        return cmd;
    }

    // ── GET metric_name from_ts to_ts ─────────────────────────────────────
    if (name == "GET")
    {
        if (tokens.size() != 4)
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "GET requires exactly 3 arguments: metric_name from_ts to_ts";
            return cmd;
        }
        if (!valid_metric_name(tokens[1]))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid metric name: " + tokens[1];
            return cmd;
        }
        int64_t from_ts, to_ts;
        if (!parse_int64(tokens[2], from_ts))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid from_timestamp: " + tokens[2];
            return cmd;
        }
        if (!parse_int64(tokens[3], to_ts))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid to_timestamp: " + tokens[3];
            return cmd;
        }
        cmd.type = CommandType::GET;
        cmd.metric_name = tokens[1];
        cmd.from_ts = from_ts;
        cmd.to_ts = to_ts;
        return cmd;
    }

    // ── AGG metric_name from_ts to_ts bucket_seconds func ─────────────────
    if (name == "AGG")
    {
        if (tokens.size() != 6)
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "AGG requires exactly 5 arguments: metric_name from_ts to_ts bucket_seconds func";
            return cmd;
        }
        if (!valid_metric_name(tokens[1]))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid metric name: " + tokens[1];
            return cmd;
        }
        int64_t from_ts, to_ts, bucket_secs;
        if (!parse_int64(tokens[2], from_ts))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid from_timestamp: " + tokens[2];
            return cmd;
        }
        if (!parse_int64(tokens[3], to_ts))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid to_timestamp: " + tokens[3];
            return cmd;
        }
        if (!parse_int64(tokens[4], bucket_secs) || bucket_secs <= 0)
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid bucket_seconds: " + tokens[4];
            return cmd;
        }
        const string &func = tokens[5];
        if (func != "avg" && func != "min" && func != "max" && func != "sum" && func != "count")
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "unknown aggregation function: " + func + " (must be avg/min/max/sum/count)";
            return cmd;
        }
        cmd.type = CommandType::AGG;
        cmd.metric_name = tokens[1];
        cmd.from_ts = from_ts;
        cmd.to_ts = to_ts;
        cmd.bucket_seconds = bucket_secs;
        cmd.agg_func = func;
        return cmd;
    }

    // ── STATS metric_name ─────────────────────────────────────────────────
    if (name == "STATS")
    {
        if (tokens.size() != 2)
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "STATS requires exactly 1 argument: metric_name";
            return cmd;
        }
        if (!valid_metric_name(tokens[1]))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid metric name: " + tokens[1];
            return cmd;
        }
        cmd.type = CommandType::STATS;
        cmd.metric_name = tokens[1];
        return cmd;
    }

    // ── FLUSH metric_name ─────────────────────────────────────────────────
    if (name == "FLUSH")
    {
        if (tokens.size() != 2)
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "FLUSH requires exactly 1 argument: metric_name";
            return cmd;
        }
        if (!valid_metric_name(tokens[1]))
        {
            cmd.type = CommandType::UNKNOWN;
            cmd.error_msg = "invalid metric name: " + tokens[1];
            return cmd;
        }
        cmd.type = CommandType::FLUSH;
        cmd.metric_name = tokens[1];
        return cmd;
    }

    // ── QUIT ──────────────────────────────────────────────────────────────
    if (name == "QUIT")
    {
        cmd.type = CommandType::QUIT;
        return cmd;
    }

    // ── Unknown ───────────────────────────────────────────────────────────
    cmd.type = CommandType::UNKNOWN;
    cmd.error_msg = "unknown command: " + name;
    return cmd;
}