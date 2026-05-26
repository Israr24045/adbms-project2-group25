#pragma once
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

#include "storage.h"

using namespace std;

class Server
{
public:
    Server(const string &data_dir, int port);
    ~Server();
    void run();

    // Retention config (set before run())
    void set_default_retention(int64_t seconds);

private:
    string data_dir_;
    int port_;
    MetricRegistry registry_;

    void handle_client(int client_fd);

    // Retention & downsampling background thread
    thread bg_thread_;
    atomic<bool> bg_running_{false};
    void background_loop();
};
