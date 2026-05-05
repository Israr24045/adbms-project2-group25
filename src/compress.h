#pragma once
#include "bitstream.h"
#include <cstdint>

// ── Timestamp — delta-of-delta encoding (Gorilla Section 4.1.1) ────────────

class TimestampEncoder {
public:
    void encode(BitWriter& bw, int64_t ts);
private:
    int64_t prev_ts_    = 0;
    int64_t prev_delta_ = 0;
    int     count_      = 0;
};

class TimestampDecoder {
public:
    int64_t decode(BitReader& br);
private:
    int64_t prev_ts_    = 0;
    int64_t prev_delta_ = 0;
    int     count_      = 0;
};

// ── Value — XOR encoding (Gorilla Section 4.1.2) ───────────────────────────

class ValueEncoder {
public:
    void encode(BitWriter& bw, double val);
private:
    uint64_t prev_bits_     = 0;
    int      prev_leading_  = 0;
    int      prev_trailing_ = 0;
    bool     first_         = true;
    bool     has_prev_xor_  = false;
};

class ValueDecoder {
public:
    double decode(BitReader& br);
private:
    uint64_t prev_bits_     = 0;
    int      prev_leading_  = 0;
    int      prev_trailing_ = 0;
    bool     first_         = true;
};