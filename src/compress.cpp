#include "compress.h"
#include <cstring>
#include <stdexcept>

// ── Helpers ────────────────────────────────────────────────────────────────

static uint64_t double_to_bits(double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    return b;
}

static double bits_to_double(uint64_t b) {
    double v;
    memcpy(&v, &b, 8);
    return v;
}

// Sign-extend an n-bit two's-complement value to int64_t
static int64_t sign_extend(uint64_t val, int n_bits) {
    uint64_t sign_bit = 1ULL << (n_bits - 1);
    if (val & sign_bit)
        return (int64_t)(val | (~0ULL << n_bits));
    return (int64_t)val;
}

// ── Delta-of-delta encoding ────────────────────────────────────────────────

static void write_dod(BitWriter& bw, int64_t D) {
    if (D == 0) {
        bw.write(0b0, 1);
    } else if (D >= -63 && D <= 64) {
        bw.write(0b10, 2);
        bw.write((uint64_t)(D & 0x7F), 7);
    } else if (D >= -255 && D <= 256) {
        bw.write(0b110, 3);
        bw.write((uint64_t)(D & 0x1FF), 9);
    } else if (D >= -2047 && D <= 2048) {
        bw.write(0b1110, 4);
        bw.write((uint64_t)(D & 0xFFF), 12);
    } else {
        bw.write(0b1111, 4);
        bw.write((uint64_t)(D & 0xFFFFFFFF), 32);
    }
}

static int64_t read_dod(BitReader& br) {
    if (br.read(1) == 0) return 0;
    if (br.read(1) == 0) return sign_extend(br.read(7),  7);
    if (br.read(1) == 0) return sign_extend(br.read(9),  9);
    if (br.read(1) == 0) return sign_extend(br.read(12), 12);
    return sign_extend(br.read(32), 32);
}

void TimestampEncoder::encode(BitWriter& bw, int64_t ts) {
    if (count_ == 0) {
        bw.write((uint64_t)ts, 64);
        prev_ts_ = ts;
        count_   = 1;
        return;
    }
    int64_t delta = ts - prev_ts_;
    if (count_ == 1) {
        // Store first delta in 14 bits (unsigned — timestamps are non-decreasing)
        bw.write((uint64_t)(delta & 0x3FFF), 14);
        prev_delta_ = delta;
        prev_ts_    = ts;
        count_      = 2;
        return;
    }
    int64_t D = delta - prev_delta_;
    write_dod(bw, D);
    prev_delta_ = delta;
    prev_ts_    = ts;
}

int64_t TimestampDecoder::decode(BitReader& br) {
    if (count_ == 0) {
        prev_ts_ = (int64_t)br.read(64);
        count_   = 1;
        return prev_ts_;
    }
    if (count_ == 1) {
        int64_t delta = (int64_t)br.read(14);
        prev_delta_   = delta;
        prev_ts_     += delta;
        count_        = 2;
        return prev_ts_;
    }
    int64_t D      = read_dod(br);
    int64_t delta  = D + prev_delta_;
    prev_ts_      += delta;
    prev_delta_    = delta;
    return prev_ts_;
}

// ── XOR value encoding ─────────────────────────────────────────────────────

void ValueEncoder::encode(BitWriter& bw, double val) {
    uint64_t bits = double_to_bits(val);

    if (first_) {
        bw.write(bits, 64);
        prev_bits_ = bits;
        first_     = false;
        return;
    }

    uint64_t xor_val = bits ^ prev_bits_;
    prev_bits_ = bits;

    if (xor_val == 0) {
        bw.write(0b0, 1);
        return;
    }

    int leading  = __builtin_clzll(xor_val);
    int trailing = __builtin_ctzll(xor_val);
    if (leading > 31) leading = 31;                   // 5-bit field max = 31
    int n_meaningful = 64 - leading - trailing;

    if (has_prev_xor_
        && leading  >= prev_leading_
        && trailing >= prev_trailing_) {
        // Reuse previous block boundaries
        bw.write(0b10, 2);
        int prev_n = 64 - prev_leading_ - prev_trailing_;
        uint64_t mbits = (prev_n == 64)
                         ? xor_val
                         : (xor_val >> prev_trailing_) & ((1ULL << prev_n) - 1);
        bw.write(mbits, prev_n);
    } else {
        // Store new block
        bw.write(0b11, 2);
        bw.write((uint64_t)leading, 5);
        // Store n_meaningful - 1 in 6 bits so range 1-64 fits (0 means 1, 63 means 64)
        bw.write((uint64_t)(n_meaningful - 1), 6);
        uint64_t mbits = (n_meaningful == 64)
                         ? xor_val
                         : (xor_val >> trailing) & ((1ULL << n_meaningful) - 1);
        bw.write(mbits, n_meaningful);
        prev_leading_  = leading;
        prev_trailing_ = trailing;
        has_prev_xor_  = true;
    }
}

double ValueDecoder::decode(BitReader& br) {
    if (first_) {
        prev_bits_ = br.read(64);
        first_     = false;
        return bits_to_double(prev_bits_);
    }

    if (br.read(1) == 0) {
        return bits_to_double(prev_bits_);
    }

    uint64_t xor_val;
    if (br.read(1) == 0) {
        // Reuse previous block
        int prev_n    = 64 - prev_leading_ - prev_trailing_;
        uint64_t mbits = br.read(prev_n);
        xor_val = (prev_n == 64) ? mbits : (mbits << prev_trailing_);
    } else {
        // New block
        int leading     = (int)br.read(5);
        int n_meaningful = (int)br.read(6) + 1;      // +1 to reverse the -1 in encoder
        int trailing    = 64 - leading - n_meaningful;
        uint64_t mbits  = br.read(n_meaningful);
        xor_val         = (n_meaningful == 64) ? mbits : (mbits << trailing);
        prev_leading_   = leading;
        prev_trailing_  = trailing;
    }

    prev_bits_ ^= xor_val;
    return bits_to_double(prev_bits_);
}