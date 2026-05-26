#include "protocol.h"

#include <sstream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdlib>

using namespace std;

// ── Fast token splitter (no istringstream, no heap vector for small counts) ──

// Find the next whitespace-delimited token starting at pos.
// Returns {token_start, token_end} or {npos, npos} if no more tokens.
static pair<size_t, size_t> next_token(const string &line, size_t pos)
{
    // skip leading spaces
    while (pos < line.size() && line[pos] == ' ')
        ++pos;
    if (pos >= line.size())
        return {string::npos, string::npos};
    size_t end = pos;
    while (end < line.size() && line[end] != ' ')
        ++end;
    return {pos, end};
}

// Split a line into whitespace-separated tokens (general fallback)
static vector<string> tokenize(const string &line)
{
    vector<string> tokens;
    size_t pos = 0;
    while (true)
    {
        auto [s, e] = next_token(line, pos);
        if (s == string::npos)
            break;
        tokens.push_back(line.substr(s, e - s));
        pos = e;
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

static bool fast_parse_int64(const char* start, const char* end, int64_t &out) {
    if (start == end) return false;
    int64_t val = 0;
    bool neg = false;
    if (*start == '-') { neg = true; ++start; }
    if (start == end) return false;
    for (const char* p = start; p < end; ++p) {
        if (*p < '0' || *p > '9') return false;
        val = val * 10 + (*p - '0');
    }
    out = neg ? -val : val;
    return true;
}

static bool fast_parse_double(const char* start, const char* end, double &out) {
    char buf[64];
    size_t len = end - start;
    if (len >= sizeof(buf) || len == 0) return false;
    memcpy(buf, start, len);
    buf[len] = '\0';
    char* endptr;
    out = strtod(buf, &endptr);
    return endptr == buf + len;
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

// ── Fast-path PUT parser ─────────────────────────────────────────────────────
// PUT is the hot path during ingestion (~500k calls).  Avoid vector/istringstream.
static Command parse_put_fast(const string &line)
{
    Command cmd;

    // Skip "PUT " (we already know line starts with "PUT")
    auto [s1, e1] = next_token(line, 4); // metric_name
    if (s1 == string::npos)
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "PUT requires exactly 3 arguments: metric_name timestamp value";
        return cmd;
    }
    string metric = line.substr(s1, e1 - s1);

    auto [s2, e2] = next_token(line, e1); // timestamp
    if (s2 == string::npos)
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "PUT requires exactly 3 arguments: metric_name timestamp value";
        return cmd;
    }

    auto [s3, e3] = next_token(line, e2); // value
    if (s3 == string::npos)
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "PUT requires exactly 3 arguments: metric_name timestamp value";
        return cmd;
    }

    // Check no extra tokens
    auto [s4, e4] = next_token(line, e3);
    if (s4 != string::npos)
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "PUT requires exactly 3 arguments: metric_name timestamp value";
        return cmd;
    }

    if (!valid_metric_name(metric))
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "invalid metric name: " + metric;
        return cmd;
    }

    int64_t ts;
    double val;
    if (!fast_parse_int64(line.data() + s2, line.data() + e2, ts))
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "invalid timestamp: " + line.substr(s2, e2 - s2);
        return cmd;
    }
    if (!fast_parse_double(line.data() + s3, line.data() + e3, val))
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "invalid value: " + line.substr(s3, e3 - s3);
        return cmd;
    }

    cmd.type = CommandType::PUT;
    cmd.metric_name = move(metric);
    cmd.timestamp = ts;
    cmd.value = val;
    return cmd;
}

Command parse_command(const string &line)
{
    Command cmd;

    // Fast-path: detect PUT early (the hot path)
    if (line.size() > 4 && line[0] == 'P' && line[1] == 'U' && line[2] == 'T' && line[3] == ' ')
    {
        return parse_put_fast(line);
    }

    vector<string> tokens = tokenize(line);

    if (tokens.empty())
    {
        cmd.type = CommandType::UNKNOWN;
        cmd.error_msg = "empty command";
        return cmd;
    }

    const string &name = tokens[0];

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
