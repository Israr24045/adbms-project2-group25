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
                        send_all(client_fd, "ERROR: head block full\n");
                        break;
                }
                break;
            }

            case CommandType::GET: {
                HeadBlock* block = registry_.get(cmd.metric_name);
                if (!block) {
                    send_all(client_fd, "(0 points)\n");
                    break;
                }
                lock_guard<mutex> lk(block->lock);
                auto range = block->range(cmd.from_ts, cmd.to_ts);
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
                string response;
                if (!block) {
                    response  = "metric: " + cmd.metric_name + "\n";
                    response += "total points: 0\n";
                    response += "in memory: 0\n";
                    response += "on disk: 0\n";
                    response += "disk chunks: 0\n";
                    response += "first timestamp: 0\n";
                    response += "last timestamp: 0\n";
                } else {
                    lock_guard<mutex> lk(block->lock);
                    size_t cnt = block->count();
                    response  = "metric: " + cmd.metric_name + "\n";
                    response += "total points: " + to_string(cnt) + "\n";
                    response += "in memory: "    + to_string(cnt) + "\n";
                    response += "on disk: 0\n";
                    response += "disk chunks: 0\n";
                    response += "first timestamp: " + to_string(block->first_timestamp()) + "\n";
                    response += "last timestamp: "  + to_string(block->last_ts()) + "\n";
                }
                send_all(client_fd, response);
                break;
            }

            case CommandType::FLUSH: {
                send_all(client_fd, "ERROR: FLUSH not implemented (Phase 2)\n");
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
