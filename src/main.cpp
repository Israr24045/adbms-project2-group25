#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>

#include "server.h"

static void print_usage(const char* prog) {
    cerr << "Usage: " << prog << " --data <data_dir> --port <port> [--retention <seconds>]\n"
              << "\n"
              << "Options:\n"
              << "  --data <dir>         Data directory for chunks and WAL files\n"
              << "  --port <port>        TCP port to listen on\n"
              << "  --retention <secs>   Global retention policy: delete chunks older than\n"
              << "                       <secs> seconds.  Background thread checks every 60s.\n"
              << "                       Set to 0 (default) to disable.\n";
}

int main(int argc, char* argv[]) {
    string data_dir;
    int port = 0;
    int64_t retention = 0;

    // Parse --data, --port, --retention from argv
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--data") == 0) {
            data_dir = argv[i + 1];
        } else if (strcmp(argv[i], "--port") == 0) {
            port = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--retention") == 0) {
            retention = atoll(argv[i + 1]);
        }
    }

    if (data_dir.empty() || port <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    cout << "tsdb starting — data: " << data_dir
              << ", port: " << port;
    if (retention > 0)
        cout << ", retention: " << retention << "s";
    cout << "\n";

    Server server(data_dir, port);

    if (retention > 0)
        server.set_default_retention(retention);

    server.run();

    return 0;
}
