#include "server.h"
#include "protocol.h"

#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <thread>
#include <iomanip>
#include <chrono>

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

static bool send_all(int fd, const string &data)
{
    const char *ptr = data.c_str();
    size_t remaining = data.size();
    while (remaining > 0)
    {
        ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent <= 0)
            return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

static string format_value(double v)
{
    if (v == static_cast<int64_t>(v))
    {
        ostringstream oss;
        oss << static_cast<int64_t>(v);
        return oss.str();
    }
    ostringstream oss;
    oss << setprecision(6) << v;
    return oss.str();
}

Server::Server(const string &data_dir, int port) : data_dir_(data_dir), port_(port) {}

Server::~Server()
{
    bg_running_ = false;
    if (bg_thread_.joinable())
        bg_thread_.join();
}

void Server::set_default_retention(int64_t seconds)
{
    registry_.set_default_retention(seconds);
}

// ── Background thread: retention enforcement + downsampling ───────────────
// Runs every 60 seconds.

void Server::background_loop()
{
    while (bg_running_)
    {
        for (int i = 0; i < 60 && bg_running_; ++i)
            this_thread::sleep_for(chrono::seconds(1));

        if (!bg_running_)
            break;

        auto now = chrono::system_clock::now();
        int64_t now_ts = chrono::duration_cast<chrono::seconds>(
                             now.time_since_epoch())
                             .count();

        // Enforce retention policies
        registry_.enforce_retention(now_ts, data_dir_);

        // Downsample old chunks
        registry_.downsample_old_chunks(data_dir_, now_ts);
    }
}

void Server::run()
{
    // Scan data directory at startup to discover existing metrics/chunks
    cout << "Scanning data directory: " << data_dir_ << "\n";
    registry_.scan_data_dir(data_dir_);

    // Replay WALs for crash recovery
    cout << "Replaying WALs for crash recovery...\n";
    registry_.replay_wals(data_dir_);

    // Start background thread for retention + downsampling
    bg_running_ = true;
    bg_thread_ = thread(&Server::background_loop, this);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        cerr << "ERROR: socket() failed: " << strerror(errno) << "\n";
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(listen_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        cerr << "ERROR: bind() failed: " << strerror(errno) << "\n";
        close(listen_fd);
        return;
    }

    if (listen(listen_fd, 64) < 0)
    {
        cerr << "ERROR: listen() failed: " << strerror(errno) << "\n";
        close(listen_fd);
        return;
    }

    cout << "tsdb listening on port " << port_ << ", data directory " << data_dir_ << "\n";

    while (true)
    {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd,
                               reinterpret_cast<struct sockaddr *>(&client_addr),
                               &client_len);
        if (client_fd < 0)
        {
            cerr << "WARN: accept() failed: " << strerror(errno) << "\n";
            continue;
        }

        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        thread(&Server::handle_client, this, client_fd).detach();
    }
}

void Server::handle_client(int client_fd)
{
    string buffer;
    size_t buffer_start = 0;
    char recv_buf[65536];   // 64 KB — read many commands per syscall

    // Response batching: accumulate responses for all commands parsed from
    // a single recv() and send them in one write.
    string resp_batch;
    resp_batch.reserve(16384);

    string last_metric;
    HeadBlock *last_block = nullptr;
    WAL *last_wal = nullptr;

    while (true)
    {
        ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n <= 0)
        {
            break;
        }
        buffer.append(recv_buf, n);

        resp_batch.clear();
        bool quit = false;

        size_t pos;
        while ((pos = buffer.find('\n', buffer_start)) != string::npos)
        {
            string line = buffer.substr(buffer_start, pos - buffer_start);
            buffer_start = pos + 1;

            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
                continue;

            Command cmd = parse_command(line);

            switch (cmd.type)
            {

            case CommandType::PUT:
            {
                HeadBlock *block;
                WAL *wal;

                if (cmd.metric_name == last_metric && last_block != nullptr) {
                    block = last_block;
                    wal = last_wal;
                } else {
                    block = registry_.get_or_create(cmd.metric_name);
                    wal = registry_.get_or_create_wal(cmd.metric_name, data_dir_);
                    last_metric = cmd.metric_name;
                    last_block = block;
                    last_wal = wal;
                }
                
                bool need_flush = false;

                {
                    lock_guard<mutex> lk(block->lock);
                    auto result = block->append(cmd.timestamp, cmd.value);
                    switch (result)
                    {
                    case HeadBlock::AppendResult::OK:
                        wal->append(cmd.timestamp, cmd.value);
                        resp_batch += "OK\n";
                        break;
                    case HeadBlock::AppendResult::OUT_OF_ORDER:
                        resp_batch += "ERROR: out-of-order timestamp\n";
                        break;
                    case HeadBlock::AppendResult::BLOCK_FULL:
                        need_flush = true;
                        break;
                    }
                }
                if (need_flush)
                {
                    // Auto-flush: flush the full head block to disk,
                    // then retry the append.  flush_metric() truncates the WAL.
                    FlushStats fs = registry_.flush_metric(cmd.metric_name, data_dir_);
                    if (fs.total_bytes > 0)
                    {
                        cerr << "auto-flush " << cmd.metric_name
                             << ": " << fs.point_count << " pts, "
                             << fs.total_bytes << " bytes\n";
                    }

                    // Re-lock and retry append
                    lock_guard<mutex> lk2(block->lock);
                    auto retry = block->append(cmd.timestamp, cmd.value);
                    if (retry == HeadBlock::AppendResult::OK)
                    {
                        wal->append(cmd.timestamp, cmd.value);
                        resp_batch += "OK\n";
                    }
                    else
                    {
                        resp_batch += "ERROR: append failed after auto-flush\n";
                    }
                }
                break;
            }

            case CommandType::GET:
            {
                auto range = registry_.full_range(cmd.metric_name, cmd.from_ts, cmd.to_ts);
                for (size_t i = 0; i < range.timestamps.size(); ++i)
                {
                    resp_batch += to_string(range.timestamps[i]) + " " + format_value(range.values[i]) + "\n";
                }
                resp_batch += "(" + to_string(range.timestamps.size()) + " points)\n";
                break;
            }

            case CommandType::STATS:
            {
                HeadBlock *block = registry_.get(cmd.metric_name);
                MetricDiskState *ds = registry_.get_disk(cmd.metric_name);

                size_t in_mem = 0;
                size_t on_disk = 0;
                size_t disk_chunks = 0;
                int64_t first_ts = 0;
                int64_t last_ts_val = 0;

                if (block)
                {
                    lock_guard<mutex> lk(block->lock);
                    in_mem = block->count();
                    if (in_mem > 0)
                    {
                        first_ts = block->first_timestamp();
                        last_ts_val = block->last_ts();
                    }
                }

                if (ds)
                {
                    lock_guard<mutex> lk(ds->lock);
                    on_disk = ds->total_disk_points;
                    disk_chunks = ds->chunks.size();
                    if (!ds->chunks.empty())
                    {
                        int64_t disk_first = ds->chunks.front().first_ts;
                        int64_t disk_last = ds->chunks.back().last_ts;
                        if (in_mem == 0 || disk_first < first_ts)
                            first_ts = disk_first;
                        if (disk_last > last_ts_val)
                            last_ts_val = disk_last;
                    }
                }

                size_t total = in_mem + on_disk;
                if (total == 0 && !block && !ds)
                {
                    first_ts = 0;
                    last_ts_val = 0;
                }

                resp_batch += "metric: " + cmd.metric_name + "\n";
                resp_batch += "total points: " + to_string(total) + "\n";
                resp_batch += "in memory: " + to_string(in_mem) + "\n";
                resp_batch += "on disk: " + to_string(on_disk) + "\n";
                resp_batch += "disk chunks: " + to_string(disk_chunks) + "\n";
                resp_batch += "first timestamp: " + to_string(first_ts) + "\n";
                resp_batch += "last timestamp: " + to_string(last_ts_val) + "\n";
                break;
            }

            case CommandType::FLUSH:
            {
                HeadBlock *block = registry_.get(cmd.metric_name);
                if (!block)
                {
                    resp_batch += "ERROR: unknown metric: " + cmd.metric_name + "\n";
                    break;
                }

                size_t pts;
                {
                    lock_guard<mutex> lk(block->lock);
                    pts = block->count();
                }

                if (pts == 0)
                {
                    resp_batch += "Nothing to flush (head block empty)\n";
                    break;
                }

                // flush_metric() also truncates the WAL
                FlushStats fs = registry_.flush_metric(cmd.metric_name, data_dir_);
                if (fs.total_bytes > 0)
                {
                    size_t naive = fs.point_count * 16;
                    double ratio = (naive > 0) ? (double)naive / (double)fs.total_bytes : 0.0;
                    ostringstream oss;
                    oss << "Flushed " << fs.point_count << " points to disk: "
                        << fs.total_bytes << " bytes"
                        << " (header " << fs.header_bytes
                        << " + ts " << fs.ts_bytes
                        << " + values " << fs.val_bytes << ")\n"
                        << "vs naive 16 bytes/point = " << naive << " bytes\n"
                        << "compression ratio: " << fixed << setprecision(2)
                        << ratio << "x"
                        << (fs.point_count < 100 ? " (small chunk, ratio improves with size)" : "")
                        << "\n";
                    resp_batch += oss.str();
                }
                else
                {
                    resp_batch += "ERROR: flush failed\n";
                }
                break;
            }

            case CommandType::AGG:
            {
                AggResult agg = registry_.agg_range(cmd.metric_name,
                                                    cmd.from_ts, cmd.to_ts,
                                                    cmd.bucket_seconds, cmd.agg_func);
                for (const auto &b : agg.buckets)
                {
                    resp_batch += to_string(b.bucket_start) + "-" + to_string(b.bucket_end) + " " + format_value(b.value) + "\n";
                }
                resp_batch += "(" + to_string(agg.buckets.size()) + " buckets)\n";
                break;
            }

            case CommandType::QUIT:
            {
                resp_batch += "BYE\n";
                quit = true;
                break;
            }

            case CommandType::UNKNOWN:
            {
                resp_batch += "ERROR: " + cmd.error_msg + "\n";
                break;
            }
            }

            if (quit) break;
        }

        // Send all accumulated responses in one syscall
        if (!resp_batch.empty())
        {
            if (!send_all(client_fd, resp_batch))
            {
                break;
            }
        }

        if (buffer_start > 0) {
            buffer.erase(0, buffer_start);
            buffer_start = 0;
        }

        if (last_wal) {
            last_wal->flush();
        }

        if (quit)
        {
            close(client_fd);
            return;
        }
    }

    close(client_fd);
}
