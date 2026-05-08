#include "server.h"
#include "protocol.h"

#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <thread>
#include <iomanip>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

static bool send_all(int fd, const string& data) {
    const char* ptr = data.c_str();
    size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= sent;
    }
    return true;
}


static string format_value(double v) {
    if (v == static_cast<int64_t>(v)) {
        ostringstream oss;
        oss << static_cast<int64_t>(v);
        return oss.str();
    }
    ostringstream oss;
    oss << setprecision(6) << v;
    return oss.str();
}


Server::Server(const string& data_dir, int port): data_dir_(data_dir), port_(port) {}

void Server::run() {
    // Scan data directory at startup to discover existing metrics/chunks
    cout << "Scanning data directory: " << data_dir_ << "\n";
    registry_.scan_data_dir(data_dir_);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        cerr << "ERROR: socket() failed: " << strerror(errno) << "\n";
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        cerr << "ERROR: bind() failed: " << strerror(errno) << "\n";
        close(listen_fd);
        return;
    }

    if (listen(listen_fd, 64) < 0) {
        cerr << "ERROR: listen() failed: " << strerror(errno) << "\n";
        close(listen_fd);
        return;
    }

    cout << "tsdb listening on port " << port_<< ", data directory " << data_dir_ << "\n";

    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd,
                                 reinterpret_cast<struct sockaddr*>(&client_addr),
                                 &client_len);
        if (client_fd < 0) {
            cerr << "WARN: accept() failed: " << strerror(errno) << "\n";
            continue;
        }

        thread(&Server::handle_client, this, client_fd).detach();
    }
}

void Server::handle_client(int client_fd) {
    string buffer;       
    char recv_buf[4096];

    while (true) {
        ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n <= 0) {
            break;
        }
        buffer.append(recv_buf, n);

        size_t pos;
        while ((pos = buffer.find('\n')) != string::npos) {
            string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) continue;

            Command cmd = parse_command(line);

            switch (cmd.type) {

            case CommandType::PUT: {
                HeadBlock* block = registry_.get_or_create(cmd.metric_name);
                bool need_flush = false;
                {
                    lock_guard<mutex> lk(block->lock);
                    auto result = block->append(cmd.timestamp, cmd.value);
                    switch (result) {
                        case HeadBlock::AppendResult::OK:
                            send_all(client_fd, "OK\n");
                            break;
                        case HeadBlock::AppendResult::OUT_OF_ORDER:
                            send_all(client_fd, "ERROR: out-of-order timestamp\n");
                            break;
                        case HeadBlock::AppendResult::BLOCK_FULL:
                            need_flush = true;
                            break;
                    }
                }
                if (need_flush) {
                    // Auto-flush: flush the full head block to disk,
                    // then retry the append.
                    FlushStats fs = registry_.flush_metric(cmd.metric_name, data_dir_);
                    if (fs.total_bytes > 0) {
                        cerr << "auto-flush " << cmd.metric_name
                             << ": " << fs.point_count << " pts, "
                             << fs.total_bytes << " bytes\n";
                    }

                    // Re-lock and retry append
                    lock_guard<mutex> lk2(block->lock);
                    auto retry = block->append(cmd.timestamp, cmd.value);
                    if (retry == HeadBlock::AppendResult::OK) {
                        send_all(client_fd, "OK\n");
                    } else {
                        send_all(client_fd, "ERROR: append failed after auto-flush\n");
                    }
                }
                break;
            }

            case CommandType::GET: {
                // Full range query: disk chunks + head block
                auto range = registry_.full_range(cmd.metric_name, cmd.from_ts, cmd.to_ts);
                string response;
                for (size_t i = 0; i < range.timestamps.size(); ++i) {
                    response += to_string(range.timestamps[i]) + " "
                              + format_value(range.values[i]) + "\n";
                }
                response += "(" + to_string(range.timestamps.size()) + " points)\n";
                send_all(client_fd, response);
                break;
            }

            case CommandType::STATS: {
                HeadBlock* block = registry_.get(cmd.metric_name);
                MetricDiskState* ds = registry_.get_disk(cmd.metric_name);

                size_t in_mem = 0;
                size_t on_disk = 0;
                size_t disk_chunks = 0;
                int64_t first_ts = 0;
                int64_t last_ts_val = 0;

                if (block) {
                    lock_guard<mutex> lk(block->lock);
                    in_mem = block->count();
                    if (in_mem > 0) {
                        first_ts = block->first_timestamp();
                        last_ts_val = block->last_ts();
                    }
                }

                if (ds) {
                    lock_guard<mutex> lk(ds->lock);
                    on_disk = ds->total_disk_points;
                    disk_chunks = ds->chunks.size();
                    if (!ds->chunks.empty()) {
                        int64_t disk_first = ds->chunks.front().first_ts;
                        int64_t disk_last  = ds->chunks.back().last_ts;
                        if (in_mem == 0 || disk_first < first_ts)
                            first_ts = disk_first;
                        if (disk_last > last_ts_val)
                            last_ts_val = disk_last;
                    }
                }

                size_t total = in_mem + on_disk;
                if (total == 0 && !block && !ds) {
                    // Metric doesn't exist at all
                    first_ts = 0;
                    last_ts_val = 0;
                }

                string response;
                response  = "metric: " + cmd.metric_name + "\n";
                response += "total points: " + to_string(total) + "\n";
                response += "in memory: "    + to_string(in_mem) + "\n";
                response += "on disk: "      + to_string(on_disk) + "\n";
                response += "disk chunks: "  + to_string(disk_chunks) + "\n";
                response += "first timestamp: " + to_string(first_ts) + "\n";
                response += "last timestamp: "  + to_string(last_ts_val) + "\n";
                send_all(client_fd, response);
                break;
            }

            case CommandType::FLUSH: {
                HeadBlock* block = registry_.get(cmd.metric_name);
                if (!block) {
                    send_all(client_fd, "ERROR: unknown metric: " + cmd.metric_name + "\n");
                    break;
                }

                size_t pts;
                {
                    lock_guard<mutex> lk(block->lock);
                    pts = block->count();
                }

                if (pts == 0) {
                    send_all(client_fd, "Nothing to flush (head block empty)\n");
                    break;
                }

                FlushStats fs = registry_.flush_metric(cmd.metric_name, data_dir_);
                if (fs.total_bytes > 0) {
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
                    send_all(client_fd, oss.str());
                } else {
                    send_all(client_fd, "ERROR: flush failed\n");
                }
                break;
            }

            case CommandType::AGG: {
                send_all(client_fd, "ERROR: AGG not implemented (Phase 3)\n");
                break;
            }

            case CommandType::QUIT: {
                send_all(client_fd, "BYE\n");
                close(client_fd);
                return;
            }

            case CommandType::UNKNOWN: {
                send_all(client_fd, "ERROR: " + cmd.error_msg + "\n");
                break;
            }

            }  
        }  
    }  

    close(client_fd);
}
