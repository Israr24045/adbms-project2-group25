#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>

#include "server.h"

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --data <data_dir> --port <port>\n";
}

int main(int argc, char* argv[]) {
    std::string data_dir;
    int port = 0;

    // Parse --data and --port from argv
    for (int i = 1; i < argc - 1; i++) {
        if (std::strcmp(argv[i], "--data") == 0) {
            data_dir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--port") == 0) {
            port = std::atoi(argv[i + 1]);
        }
    }

    if (data_dir.empty() || port <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "tsdb starting — data: " << data_dir
              << ", port: " << port << "\n";

    Server server(data_dir, port);
    server.run();

    return 0;
}