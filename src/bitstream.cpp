#include "bitstream.h"
#include <stdexcept>

void BitWriter::write(uint64_t value, int n_bits) {
    while (n_bits > 0) {
        int free_in_byte = 8 - bits_filled_;
        int take         = (n_bits < free_in_byte) ? n_bits : free_in_byte;
        int shift        = n_bits - take;
        uint64_t chunk   = (value >> shift) & ((1ULL << take) - 1);
        current_byte_   |= (uint8_t)(chunk << (free_in_byte - take));
        bits_filled_    += take;
        n_bits          -= take;
        if (bits_filled_ == 8) {
            buf_.push_back(current_byte_);
            current_byte_ = 0;
            bits_filled_  = 0;
        }
    }
}

void BitWriter::flush() {
    if (bits_filled_ > 0) {
        buf_.push_back(current_byte_);
        current_byte_ = 0;
        bits_filled_  = 0;
    }
}

uint64_t BitReader::read(int n_bits) {
    uint64_t result = 0;
    while (n_bits > 0) {
        if (byte_pos_ >= len_)
            throw std::runtime_error("BitReader: read past end of buffer");
        int avail    = 8 - bits_consumed_;
        int take     = (n_bits < avail) ? n_bits : avail;
        int shift    = avail - take;
        uint8_t chunk = (buf_[byte_pos_] >> shift) & ((1 << take) - 1);
        result        = (result << take) | chunk;
        bits_consumed_ += take;
        n_bits         -= take;
        if (bits_consumed_ == 8) {
            byte_pos_++;
            bits_consumed_ = 0;
        }
    }
    return result;
}