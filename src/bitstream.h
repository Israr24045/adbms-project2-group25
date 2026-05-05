#pragma once
#include <cstdint>
#include <vector>

class BitWriter {
public:
    void write(uint64_t value, int n_bits);
    void flush();
    const std::vector<uint8_t>& data() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
    uint8_t current_byte_ = 0;
    int     bits_filled_  = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* buf, size_t len)
        : buf_(buf), len_(len), byte_pos_(0), bits_consumed_(0) {}

    uint64_t read(int n_bits);

private:
    const uint8_t* buf_;
    size_t         len_;
    size_t         byte_pos_;
    int            bits_consumed_;
};