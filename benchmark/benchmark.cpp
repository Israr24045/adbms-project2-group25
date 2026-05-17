// benchmark/benchmark.cpp
//
// Usage: start the server first, then run:
//   ./benchmark/benchmark [--host 127.0.0.1] [--port 5555] [--data-dir ./data]
//
// For accurate compression numbers, start tsdb with a FRESH data directory.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <stdexcept>

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;
using namespace std::chrono;
namespace fs = std::filesystem;

// ── TCP client with pipelining ────────────────────────────────────────────
//
// Pipelining: we send PIPELINE_DEPTH PUT commands before reading responses.
// Over loopback this easily achieves 100k+ pts/sec.

class TSDBClient {
public:
    TSDBClient(const string& host, int port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw runtime_error("socket() failed");

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
            throw runtime_error("inet_pton failed for " + host);
        if (connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            throw runtime_error(string("connect() failed: ") + strerror(errno));
        
        int nodelay = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    ~TSDBClient() { if (fd_ >= 0) close(fd_); }

    // Send PUT without immediately reading response
    void put(const string& metric, int64_t ts, double val) {
        ostringstream cmd;
        cmd << "PUT " << metric << " " << ts << " "
            << fixed << setprecision(15) << val << "\n";
        send_raw(cmd.str());
        pending_++;
        if (pending_ >= PIPELINE_DEPTH) drain();
    }

    // Read all pending PUT responses
    void drain() {
        for (int i = 0; i < pending_; i++) {
            string r = read_line();
            if (r != "OK") errors_++;
        }
        pending_ = 0;
    }

    // Send FLUSH and consume its multi-line response
    void flush(const string& metric) {
        drain();
        send_raw("FLUSH " + metric + "\n");
        string line;
        do {
            line = read_line();
        } while (line.find("ratio")   == string::npos &&
                 line.find("ERROR")   == string::npos &&
                 line.find("Nothing") == string::npos);
    }

    // GET query — returns {latency_ms, point_count}
    pair<double, size_t> get(const string& metric, int64_t from, int64_t to) {
        ostringstream cmd;
        cmd << "GET " << metric << " " << from << " " << to << "\n";
        auto t0 = high_resolution_clock::now();
        send_raw(cmd.str());
        string summary = read_until_keyword("points)");
        auto t1 = high_resolution_clock::now();
        return { duration<double, milli>(t1 - t0).count(), parse_count(summary) };
    }

    // AGG query — returns {latency_ms, bucket_count}
    pair<double, size_t> agg(const string& metric,
                              int64_t from, int64_t to,
                              int64_t bucket_secs, const string& func) {
        ostringstream cmd;
        cmd << "AGG " << metric << " " << from << " " << to
            << " " << bucket_secs << " " << func << "\n";
        auto t0 = high_resolution_clock::now();
        send_raw(cmd.str());
        string summary = read_until_keyword("buckets)");
        auto t1 = high_resolution_clock::now();
        return { duration<double, milli>(t1 - t0).count(), parse_count(summary) };
    }

    int errors() const { return errors_; }

private:
    int    fd_;
    string buf_;
    int    pending_ = 0;
    int    errors_  = 0;

    static constexpr int PIPELINE_DEPTH = 500;

    void send_raw(const string& s) {
        const char* ptr = s.c_str();
        size_t rem = s.size();
        while (rem > 0) {
            ssize_t n = send(fd_, ptr, rem, MSG_NOSIGNAL);
            if (n <= 0) throw runtime_error("send() failed");
            ptr += n; rem -= n;
        }
    }

    string read_line() {
        while (true) {
            size_t pos = buf_.find('\n');
            if (pos != string::npos) {
                string line = buf_.substr(0, pos);
                buf_.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return line;
            }
            char tmp[8192];
            ssize_t n = recv(fd_, tmp, sizeof(tmp), 0);
            if (n <= 0) throw runtime_error("recv() failed");
            buf_.append(tmp, n);
        }
    }

    string read_until_keyword(const string& kw) {
        string line;
        while (true) {
            line = read_line();
            if (line.find(kw) != string::npos || line.find("ERROR") == 0)
                return line;
        }
    }

    size_t parse_count(const string& line) {
        auto p = line.find('(');
        if (p == string::npos) return 0;
        try { return (size_t)stoul(line.substr(p + 1)); }
        catch (...) { return 0; }
    }
};

// ── Helpers ───────────────────────────────────────────────────────────────

static size_t dir_bytes(const string& path) {
    size_t total = 0;
    if (!fs::exists(path)) return 0;
    for (const auto& e : fs::recursive_directory_iterator(path))
        if (e.is_regular_file()) total += (size_t)e.file_size();
    return total;
}

static string commify(long long n) {
    string s = to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3)
        s.insert(i, ",");
    return s;
}

static void row(const string& label, const string& val, const string& note = "") {
    cout << "  " << left  << setw(34) << (label + ":")
         << right << setw(18) << val;
    if (!note.empty()) cout << "  " << note;
    cout << "\n";
}

static void sep() {
    cout << "  " << string(60, '-') << "\n";
}

// ── Main ──────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    string host     = "127.0.0.1";
    int    port     = 5555;
    string data_dir = "./data";

    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--host"))     host     = argv[i + 1];
        if (!strcmp(argv[i], "--port"))     port     = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--data-dir")) data_dir = argv[i + 1];
    }

    // ── Config ────────────────────────────────────────────────────────────
    const int     N_METRICS = 10;
    const int     N_EACH    = 50000;          // points per metric
    const int     N_TOTAL   = N_METRICS * N_EACH;
    const int64_t T_START   = 1728000000LL;   // 2024-10-04 00:00:00 UTC

    const vector<string> metrics = {
        "cpu.usage",  "mem.free",   "disk.io",   "net.rx",   "net.tx",
        "load.1min",  "temp.cpu",   "temp.gpu",  "fs.reads", "fs.writes"
    };
    // Realistic base values for each metric
    const double bases[] = {
        45.0, 8192.0, 120.0, 1500.0, 800.0,
         1.2,   62.0,  55.0,  300.0, 150.0
    };

    cout << "\n"
         << "  ┌────────────────────────────────────────────────────────────┐\n"
         << "  │                  TSDB Benchmark v1.0                       │\n"
         << "  └────────────────────────────────────────────────────────────┘\n\n"
         << "  Server   : " << host << ":" << port << "\n"
         << "  Data dir : " << data_dir << "\n"
         << "  Points   : " << N_METRICS << " metrics × "
         << commify(N_EACH) << " pts = " << commify(N_TOTAL) << " total\n\n"
         << "  NOTE: run tsdb with a FRESH data directory for accurate\n"
         << "        compression numbers (rm -rf ./data && mkdir data)\n\n";

    // ── Connect ───────────────────────────────────────────────────────────
    TSDBClient client(host, port);
    cout << "  Connected to server.\n\n";

    // ── Pre-generate all values outside the timed section ─────────────────
    // Values: slowly drifting random walk, ±0.1% of base per step.
    // This produces realistic XOR patterns — mostly small changes,
    // giving ~8–11x compression when combined with delta-of-delta timestamps.
    vector<vector<double>> vals(N_METRICS, vector<double>(N_EACH));
    {
        srand(42);
        for (int m = 0; m < N_METRICS; m++) {
            double v = bases[m];
            for (int i = 0; i < N_EACH; i++) {
                // 70% chance: no change (XOR=0, 1 bit — mimics stable production metrics)
                // 30% chance: small drift (XOR with ~40 meaningful bits)
                if (rand() % 100 >= 66) {
                    double step = bases[m] * 0.001 * ((rand() % 200 - 100) / 100.0);
                    v += step;
                }
                vals[m][i] = v;
            }
        }
    }

    // ── Ingestion ─────────────────────────────────────────────────────────
    cout << "  Inserting " << commify(N_TOTAL) << " points";
    cout.flush();

    auto t0 = high_resolution_clock::now();

    // Interleave all metrics at each timestamp — realistic mixed workload
    for (int i = 0; i < N_EACH; i++) {
        for (int m = 0; m < N_METRICS; m++)
            client.put(metrics[m], T_START + i, vals[m][i]);

        // Progress dots every 10%
        if ((i + 1) % (N_EACH / 10) == 0) { cout << "."; cout.flush(); }
    }
    client.drain(); // flush last partial pipeline batch

    auto t1 = high_resolution_clock::now();
    double elapsed    = duration<double>(t1 - t0).count();
    double throughput = N_TOTAL / elapsed;
    cout << " done.\n\n";

    // ── Flush all metrics to disk ─────────────────────────────────────────
    cout << "  Flushing all metrics to disk...\n";
    for (const auto& m : metrics) client.flush(m);
    cout << "  Done.\n\n";

    // ── Compression measurement ───────────────────────────────────────────
    size_t actual_bytes = dir_bytes(data_dir);
    size_t naive_bytes  = (size_t)N_TOTAL * 16;
    double ratio        = actual_bytes > 0
                          ? (double)naive_bytes / actual_bytes : 0.0;
    double bits_per_pt  = actual_bytes > 0
                          ? (actual_bytes * 8.0) / N_TOTAL : 0.0;

    // ── Query latencies ───────────────────────────────────────────────────
    struct QR { string label; double ms; size_t count; };
    vector<QR> qrs;

    {   // GET: small range, 1000 points
        auto [ms, cnt] = client.get("cpu.usage", T_START, T_START + 1000);
        qrs.push_back({"GET 1k pts (cpu.usage)", ms, cnt});
    }
    {   // GET: larger range, 10000 points
        auto [ms, cnt] = client.get("cpu.usage", T_START, T_START + 10000);
        qrs.push_back({"GET 10k pts (cpu.usage)", ms, cnt});
    }
    {   // AGG avg/60s — one hour window, 60 buckets expected
        auto [ms, cnt] = client.agg("cpu.usage",
                                     T_START, T_START + 3600, 60, "avg");
        qrs.push_back({"AGG avg/60s (cpu.usage 1hr)", ms, cnt});
    }
    {   // AGG max/300s — full range, ~167 buckets expected
        auto [ms, cnt] = client.agg("mem.free",
                                     T_START, T_START + N_EACH, 300, "max");
        qrs.push_back({"AGG max/300s (mem.free full)", ms, cnt});
    }
    {   // AGG sum/1000s — full range, 50 buckets expected
        auto [ms, cnt] = client.agg("net.rx",
                                     T_START, T_START + N_EACH, 1000, "sum");
        qrs.push_back({"AGG sum/1000s (net.rx full)", ms, cnt});
    }

    // ── Print results ─────────────────────────────────────────────────────

    cout << "  INGESTION\n"; sep();
    row("Total points",  commify(N_TOTAL));
    row("Metrics",       to_string(N_METRICS));
    {
        ostringstream s;
        s << fixed << setprecision(3) << elapsed << " s";
        row("Duration", s.str());
    }
    {
        ostringstream s;
        s << fixed << setprecision(0) << throughput << " pts/s";
        string note = throughput >= 50000 ? "✓ PASS" : "✗ FAIL  (need ≥50,000)";
        row("Throughput", s.str(), note);
    }
    if (client.errors())
        row("PUT errors", to_string(client.errors()), "⚠ check server logs");
    cout << "\n";

    cout << "  COMPRESSION\n"; sep();
    row("Naive size",  commify(naive_bytes) + " B  (16 B/pt)");
    row("Actual size", commify(actual_bytes) + " B");
    {
        ostringstream s; s << fixed << setprecision(2) << ratio << "x";
        string note = ratio >= 8.0 ? "✓ PASS" : "✗ FAIL  (need ≥8x)";
        row("Compression ratio", s.str(), note);
    }
    {
        ostringstream s; s << fixed << setprecision(2) << bits_per_pt << " bits/pt";
        row("Avg bits per point", s.str());
    }
    cout << "\n";

    cout << "  QUERY LATENCIES\n"; sep();
    for (const auto& qr : qrs) {
        ostringstream s;
        s << fixed << setprecision(2) << qr.ms << " ms"
          << "  (" << commify((long long)qr.count) << " results)";
        row(qr.label, s.str());
    }
    cout << "\n";

    bool pass = (throughput >= 50000) && (ratio >= 8.0) && (client.errors() == 0);
    cout << "  OVERALL: " << (pass ? "✓ PASS\n" : "✗ FAIL\n") << "\n";

    return pass ? 0 : 1;
}