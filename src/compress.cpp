#include "compress.h"
#include <cstring>

static uint64_t double_to_bits(double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    return b;
}
static double bits_to_double(uint64_t b)
{
    double v;
    memcpy(&v, &b, 8);
    return v;
}
static int64_t sign_extend(uint64_t val, int n_bits)
{
    uint64_t sign_bit = 1ULL << (n_bits - 1);
    if (val & sign_bit)
        return (int64_t)(val | (~0ULL << n_bits));
    return (int64_t)val;
}

static void write_dod(BitWriter &bw, int64_t D)
{
    if (D == 0)
    {
        bw.write(0b0, 1);
    }
    else if (D >= -63 && D <= 64)
    {
        bw.write(0b10, 2);
        bw.write((uint64_t)(D & 0x7F), 7);
    }
    else if (D >= -255 && D <= 256)
    {
        bw.write(0b110, 3);
        bw.write((uint64_t)(D & 0x1FF), 9);
    }
    else if (D >= -2047 && D <= 2048)
    {
        bw.write(0b1110, 4);
        bw.write((uint64_t)(D & 0xFFF), 12);
    }
    else
    {
        bw.write(0b1111, 4);
        bw.write((uint64_t)(D & 0xFFFFFFFF), 32);
    }
}
static int64_t read_dod(BitReader &br)
{
    if (br.read(1) == 0)
        return 0;
    if (br.read(1) == 0)
        return sign_extend(br.read(7), 7);
    if (br.read(1) == 0)
        return sign_extend(br.read(9), 9);
    if (br.read(1) == 0)
        return sign_extend(br.read(12), 12);
    return sign_extend(br.read(32), 32);
}

void TimestampEncoder::encode(BitWriter &bw, int64_t ts)
{
    if (count_ == 0)
    {
        bw.write((uint64_t)ts, 64);
        prev_ts_ = ts;
        count_ = 1;
        return;
    }
    int64_t delta = ts - prev_ts_;
    if (count_ == 1)
    {
        bw.write((uint64_t)(delta & 0x3FFF), 14);
        prev_delta_ = delta;
        prev_ts_ = ts;
        count_ = 2;
        return;
    }
    write_dod(bw, delta - prev_delta_);
    prev_delta_ = delta;
    prev_ts_ = ts;
}
int64_t TimestampDecoder::decode(BitReader &br)
{
    if (count_ == 0)
    {
        prev_ts_ = (int64_t)br.read(64);
        count_ = 1;
        return prev_ts_;
    }
    if (count_ == 1)
    {
        int64_t d = (int64_t)br.read(14);
        prev_delta_ = d;
        prev_ts_ += d;
        count_ = 2;
        return prev_ts_;
    }
    int64_t delta = read_dod(br) + prev_delta_;
    prev_ts_ += delta;
    prev_delta_ = delta;
    return prev_ts_;
}

// ── XOR value encoding ─────────────────────────────────────────────────────
//
// Reuse condition: current XOR bits fit EXACTLY inside the stored window.
// All three must hold:
//   leading  >= prev_leading_   (window starts no earlier from MSB)
//   trailing >= prev_trailing_  (window ends no later from LSB  — critical!)
//   This implies n_meaningful <= prev_n_meaningful_ automatically.
//
// Without trailing >= prev_trailing_, right-shifting by prev_trailing_ loses
// LSBs of the current XOR that sit below the stored window boundary.

void ValueEncoder::encode(BitWriter &bw, double val)
{
    uint64_t bits = double_to_bits(val);
    if (first_)
    {
        bw.write(bits, 64);
        prev_bits_ = bits;
        first_ = false;
        return;
    }

    uint64_t xor_val = bits ^ prev_bits_;
    prev_bits_ = bits;

    if (xor_val == 0)
    {
        bw.write(0b0, 1);
        return;
    }

    int leading = __builtin_clzll(xor_val);
    int trailing = __builtin_ctzll(xor_val);
    if (leading > 31)
        leading = 31;
    int n_meaningful = 64 - leading - trailing;

    // All three conditions required for correct lossless reuse
    bool reuse = has_prev_xor_ && (leading >= prev_leading_) && (trailing >= prev_trailing_);

    if (reuse)
    {
        bw.write(0b10, 2);
        uint64_t mbits = (prev_n_meaningful_ == 64)
                             ? xor_val
                             : (xor_val >> prev_trailing_) & ((1ULL << prev_n_meaningful_) - 1);
        bw.write(mbits, prev_n_meaningful_);
        // Do NOT update state on reuse
    }
    else
    {
        bw.write(0b11, 2);
        bw.write((uint64_t)leading, 5);
        bw.write((uint64_t)(n_meaningful - 1), 6);
        uint64_t mbits = (n_meaningful == 64)
                             ? xor_val
                             : (xor_val >> trailing) & ((1ULL << n_meaningful) - 1);
        bw.write(mbits, n_meaningful);
        prev_leading_ = leading;
        prev_trailing_ = trailing;
        prev_n_meaningful_ = n_meaningful;
        has_prev_xor_ = true;
    }
}

double ValueDecoder::decode(BitReader &br)
{
    if (first_)
    {
        prev_bits_ = br.read(64);
        first_ = false;
        return bits_to_double(prev_bits_);
    }
    if (br.read(1) == 0)
        return bits_to_double(prev_bits_);

    uint64_t xor_val;
    if (br.read(1) == 0)
    {
        uint64_t mbits = br.read(prev_n_meaningful_);
        xor_val = (prev_n_meaningful_ == 64) ? mbits : (mbits << prev_trailing_);
    }
    else
    {
        int leading = (int)br.read(5);
        int n_meaningful = (int)br.read(6) + 1;
        int trailing = 64 - leading - n_meaningful;
        uint64_t mbits = br.read(n_meaningful);
        xor_val = (n_meaningful == 64) ? mbits : (mbits << trailing);
        prev_leading_ = leading;
        prev_trailing_ = trailing;
        prev_n_meaningful_ = n_meaningful;
    }
    prev_bits_ ^= xor_val;
    return bits_to_double(prev_bits_);
}