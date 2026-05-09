#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include "bitstream.h"
#include "compress.h"

static bool bits_equal(double a, double b)
{
    uint64_t ba, bb;
    memcpy(&ba, &a, 8);
    memcpy(&bb, &b, 8);
    return ba == bb;
}

static int test_timestamps()
{
    const int N = 1000;
    std::vector<int64_t> ts(N);
    for (int i = 0; i < N; i++)
        ts[i] = 1728000000LL + i * 10;

    BitWriter bw;
    TimestampEncoder enc;
    for (int i = 0; i < N; i++)
        enc.encode(bw, ts[i]);
    bw.flush();

    const auto &buf = bw.data();
    BitReader br(buf.data(), buf.size());
    TimestampDecoder dec;

    int fail = 0;
    for (int i = 0; i < N; i++)
    {
        int64_t got = dec.decode(br);
        if (got != ts[i])
        {
            std::cerr << "TS FAIL i=" << i
                      << " expected=" << ts[i]
                      << " got=" << got << "\n";
            if (++fail > 5)
                break;
        }
    }

    double bits_per_point = (buf.size() * 8.0) / N;
    if (fail == 0)
    {
        std::cout << "PASS: timestamp encoder/decoder ("
                  << N << " points, "
                  << bits_per_point << " bits/point)\n";
        return 0;
    }
    std::cout << "FAIL: timestamp (" << fail << " mismatches)\n";
    return 1;
}

static int test_values()
{
    const int N = 1000;
    std::vector<double> vals(N);
    double v = 45.0;
    srand(42);
    for (int i = 0; i < N; i++)
    {
        v += ((double)(rand() % 20 - 10)) / 10000.0;
        vals[i] = v;
    }

    BitWriter bw;
    ValueEncoder enc;
    for (int i = 0; i < N; i++)
        enc.encode(bw, vals[i]);
    bw.flush();

    const auto &buf = bw.data();
    BitReader br(buf.data(), buf.size());
    ValueDecoder dec;

    int fail = 0;
    for (int i = 0; i < N; i++)
    {
        double got = dec.decode(br);
        if (!bits_equal(got, vals[i]))
        {
            std::cerr << "VAL FAIL i=" << i
                      << " expected=" << vals[i]
                      << " got=" << got << "\n";
            if (++fail > 5)
                break;
        }
    }

    double bits_per_point = (buf.size() * 8.0) / N;
    if (fail == 0)
    {
        std::cout << "PASS: value encoder/decoder ("
                  << N << " points, "
                  << bits_per_point << " bits/point)\n";
        return 0;
    }
    std::cout << "FAIL: value (" << fail << " mismatches)\n";
    return 1;
}

int main()
{
    int r = 0;
    r += test_timestamps();
    r += test_values();
    return r;
}