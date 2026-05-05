#include <iostream>
#include <vector>
#include <cstdlib>
#include "bitstream.h"

int main() {
    srand(42);
    const int N = 1000;

    std::vector<uint64_t> values(N);
    std::vector<int>      widths(N);

    BitWriter bw;
    for (int i = 0; i < N; i++) {
        widths[i]  = 1 + rand() % 64;
        uint64_t mask = (widths[i] == 64)
                        ? ~0ULL
                        : (1ULL << widths[i]) - 1;
        values[i]  = ((uint64_t)rand() ^ ((uint64_t)rand() << 32)) & mask;
        bw.write(values[i], widths[i]);
    }
    bw.flush();

    const auto& buf = bw.data();
    BitReader br(buf.data(), buf.size());

    int fail = 0;
    for (int i = 0; i < N; i++) {
        uint64_t got = br.read(widths[i]);
        if (got != values[i]) {
            std::cerr << "FAIL i=" << i
                      << " width=" << widths[i]
                      << " expected=" << values[i]
                      << " got=" << got << "\n";
            if (++fail > 5) break;
        }
    }

    if (fail == 0) {
        std::cout << "PASS: bitstream round-trip (" << N << "/" << N << ")\n";
        return 0;
    }
    std::cout << "FAIL: " << fail << " mismatches\n";
    return 1;
}