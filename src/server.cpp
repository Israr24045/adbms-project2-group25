#include "server.h"
#include <iostream>

Server::Server(const std::string& data_dir, int port)
    : data_dir_(data_dir), port_(port) {}

void Server::run() {
    // Placeholder — TCP logic comes later
    std::cout << "tsdb started on port " << port_
              << ", data directory " << data_dir_ << "\n";

    // Block forever for now so the process doesn't exit
    // Will replace this with the real accept loop
    while (true) {}
}